/*
 * bufpool.cc -- the buffer pool of a tree's pages (bufpool.h,
 * ../BUFMGR.md §1).
 *
 * A frame's state word is its pin: a count of shared pins, an
 * exclusive bit, and a bit saying a thread sleeps on it. Its tag lies
 * in relaxed atomics, so a lookup reads it with no lock; only a pin
 * makes it sure, as a tag changes only under an exclusive pin. So a
 * lookup finds a frame, pins it, and compares the tag again.
 *
 * Buckets are chains of frame indexes. A chain changes under its
 * bucket's lock and is read with none: a reader on a frame that moves
 * walks on into another chain, and misses. A miss looks again under
 * the lock before it takes the page a frame, so a page has one frame.
 */

#include "bufpool.h"

#include "error.h"

#include "sm_sys.h"

#include <assert.h>
#include <string.h>
#include <unistd.h>

using namespace wisr;

namespace {

/* The state word's bits; below them, the count of shared pins. */
constexpr uint32_t state_ex = 1u << 31;
constexpr uint32_t state_wait = 1u << 30;

/* A tag's len while a frame holds no page. */
constexpr uint32_t no_page = UINT32_MAX;

/* The bit a pin sets in a frame's age. */
constexpr uint8_t age_pinned = 128;

/* How often a pin looks at a frame before it sleeps. */
constexpr int spins = 32;

/* How long a pin sleeps while every frame is pinned. */
constexpr useconds_t victim_sleep_us = 200;

/* A word of the bucket hash: Murmur3's finalizer. */
uint64_t mix(uint64_t h) {
  h ^= h >> 33;
  h *= 0xff51afd7ed558ccdULL;
  h ^= h >> 33;
  h *= 0xc4ceb9fe1a85ec53ULL;
  return h ^ (h >> 33);
}

/* The condition of one sleep: false before it, true once woken. */
bool woke(void *arg) {
  bool *slept = static_cast<bool *>(arg);
  bool was = *slept;
  *slept = true;
  return was;
}

/* Fail as a wait a bit in the thread's mask ended. */
int interrupted() {
  return set_error(sm1::SM_ERR_INTERRUPTED, "a wait for a page interrupted");
}

} // namespace

/* A page's tag, its key in two words. */
struct bufpool_t::tag_t {
  uint64_t dbid = 0;
  uint64_t storeid = 0;
  uint64_t k0 = 0;
  uint64_t k1 = 0;
  uint32_t len = no_page;

  tag_t() = default;
  /* The tag of page, whose key fits. */
  explicit tag_t(const bufpool_page_t &page)
      : dbid(page.dbid), storeid(page.storeid),
        len(static_cast<uint32_t>(page.key.size())) {
    char k[bufpool_key_max] = {};
    memcpy(k, page.key.data(), page.key.size());
    memcpy(&k0, k, 8);
    memcpy(&k1, k + 8, 8);
  }
  bool operator==(const tag_t &) const = default;
};

/* A frame's descriptor, in a cache line of its own. */
struct alignas(64) bufpool_t::frame_t {
  std::atomic<uint32_t> state{0};
  std::atomic<int32_t> next{-1};  /* the next in its chain */
  std::atomic<uint8_t> age{0};    /* the clock's: a pin sets its top bit */
  std::atomic<bool> inner{false}; /* its last pin's bufpool_inner */
  /* Under the pin; its bytes are bufpool_t::bytes_of's. */
  bool valid = false; /* bytes hold the page */
  uint16_t size = 0;
  /* The tag, written under an exclusive pin. */
  std::atomic<uint64_t> dbid{0};
  std::atomic<uint64_t> storeid{0};
  std::atomic<uint64_t> k0{0};
  std::atomic<uint64_t> k1{0};
  std::atomic<uint32_t> len{no_page};
  /* Its slot in m_copies holds a copy: under a pin. */
  std::atomic<bool> copied{false};
  /*
   * Moved by each change of the tag or the bytes, under an exclusive
   * pin, so a copy of a page knows the frame since (bufpool.h).
   */
  std::atomic<uint64_t> version{0};

  /* The tag as it is now: sure only under a pin. */
  tag_t tag() const {
    tag_t t;
    t.dbid = dbid.load(std::memory_order_relaxed);
    t.storeid = storeid.load(std::memory_order_relaxed);
    t.k0 = k0.load(std::memory_order_relaxed);
    t.k1 = k1.load(std::memory_order_relaxed);
    t.len = len.load(std::memory_order_relaxed);
    return t;
  }
  /* Set the tag, under an exclusive pin. */
  void set_tag(const tag_t &t) {
    version.fetch_add(1, std::memory_order_release);
    dbid.store(t.dbid, std::memory_order_relaxed);
    storeid.store(t.storeid, std::memory_order_relaxed);
    k0.store(t.k0, std::memory_order_relaxed);
    k1.store(t.k1, std::memory_order_relaxed);
    len.store(t.len, std::memory_order_relaxed);
  }
};

/* Where threads sleep on frames: one a few frames, frames sharing it. */
struct bufpool_t::waitq_t {
  std::mutex mu;
  std::condition_variable cv;
  /*
   * The threads asleep in cv, or woken and yet to take mu again, on any
   * of the frames this queue serves. Read and written under mu only.
   */
  int sleepers = 0;
};

/* Make the frames, each empty, and the buckets, twice as many. */
bufpool_t::bufpool_t(size_t frames)
    : m_nframes(frames), m_frames(new frame_t[frames]),
      m_mem(new char[frames * bufpool_frame_bytes]),
      m_copies(new std::atomic<std::shared_ptr<const bufpool_copy_t>>[frames]),
      m_waitqs(new waitq_t[waitqs]) {
  static_assert(64 == sizeof(frame_t), "a frame's descriptor is one line");
  assert(frames > 0);
  size_t buckets = 16;
  while (buckets < 2 * frames) {
    buckets *= 2;
  }
  m_mask = buckets - 1;
  m_buckets.reset(new std::atomic<int32_t>[buckets]);
  for (size_t i = 0; i < buckets; i++) {
    m_buckets[i].store(-1, std::memory_order_relaxed);
  }
  m_locks.reset(new std::mutex[bucket_locks]);
}

/* Free the frames; no pin may be held. */
bufpool_t::~bufpool_t() = default;

/* The chain a tag lies in. */
size_t bufpool_t::bucket_of(const tag_t &t) const {
  uint64_t h = mix(t.dbid ^ mix(t.storeid ^ mix(t.k0 ^ mix(t.k1 + t.len))));
  return h & m_mask;
}

/* The bytes of f, its page's when valid. */
char *bufpool_t::bytes_of(const frame_t *f) const {
  return m_mem.get() +
         static_cast<size_t>(f - m_frames.get()) * bufpool_frame_bytes;
}

/* The queue threads sleep on for f. */
bufpool_t::waitq_t &bufpool_t::waitq_of(const frame_t *f) const {
  return m_waitqs[static_cast<size_t>(f - m_frames.get()) % waitqs];
}

/*
 * The frame whose tag is tag now, with no lock: it may change before
 * it is pinned, and a chain walked as it changes may miss one that is
 * there. nullptr if none was seen.
 */
bufpool_t::frame_t *bufpool_t::find(const tag_t &tag) const {
  int32_t i = m_buckets[bucket_of(tag)].load(std::memory_order_acquire);
  // A frame moving between chains could lead a walk round: a miss.
  for (size_t steps = 0; i >= 0 && steps < m_nframes; steps++) {
    frame_t *f = &m_frames[static_cast<size_t>(i)];
    if (f->tag() == tag) {
      return f;
    }
    i = f->next.load(std::memory_order_acquire);
  }
  return nullptr;
}

namespace {

/*
 * Pin state's frame if it can be, now. A pin that is yet to wait
 * leaves way for a thread asleep, past_wait false, so a writer is not
 * starved by readers; one that has slept takes its turn.
 */
bool try_pin(std::atomic<uint32_t> &state, bool ex, bool past_wait) {
  uint32_t s = state.load(std::memory_order_relaxed);
  for (;;) {
    if (!past_wait && 0 != (s & state_wait)) {
      return false;
    }
    uint32_t held = s & ~state_wait;
    if (ex ? 0 != held : 0 != (s & state_ex)) {
      return false;
    }
    if (state.compare_exchange_weak(s, ex ? s | state_ex : s + 1,
                                    std::memory_order_acquire,
                                    std::memory_order_relaxed)) {
      return true;
    }
  }
}

} // namespace

/*
 * Pin f, found for tag, waiting by sm_wait: 0; 1 if f holds another
 * page by now, nothing pinned; -1 if a wait was interrupted.
 * 1. Spin on the fast path, which yields to a sleeper's wait bit.
 * 2. Take the wait queue's mu, held from here to the wait.
 * 3. Set the wait bit, so a release after it wakes this thread.
 * 4. Try the pin, past the bit. On success, clear the bit if no
 *    thread sleeps on the queue: else it outlasts the sleep it
 *    stood for, and sends the page's next pins to the slow path.
 * 5. If f holds another page by now, return 1.
 * 6. Sleep, counted in the queue's sleepers; then go to step 3.
 */
int bufpool_t::acquire(frame_t *f, bool ex, const tag_t &tag) {
  for (int i = 0; i < spins; i++) {
    if (try_pin(f->state, ex, false)) {
      return 0;
    }
  }
  // Step 2. A pin's wait may last through a write that stalls
  // (../BUFMGR.md §1.3), so it sleeps, and can be cut short.
  waitq_t &q = waitq_of(f);
  std::unique_lock<std::mutex> guard(q.mu);
  for (;;) {
    // Step 3: the bit first, then the try.
    f->state.fetch_or(state_wait, std::memory_order_relaxed);
    if (try_pin(f->state, ex, true)) {
      // Step 4. With mu held, every thread that set the bit and has
      // yet to take its pin is in sleepers, so a count of 0 means none
      // needs the bit. A sleeper on another frame of the queue keeps
      // the bit too: a cost, never a lost wake-up.
      if (0 == q.sleepers) {
        f->state.fetch_and(~state_wait, std::memory_order_relaxed);
      }
      return 0;
    }
    // Step 5. Never sleep on another page's frame: its holder may wait
    // for a page this thread holds, out of the order pages are latched
    // in.
    if (!(f->tag() == tag)) {
      return 1;
    }
    // Step 6. Counted until it has mu again, woken or interrupted.
    bool slept = false;
    q.sleepers++;
    sm_wait_result_t r =
        sm_wait(&guard, &q.cv, woke, &slept, sm_wait_mask(), SM_FOREVER);
    q.sleepers--;
    if (SM_WAIT_INTERRUPTED == r) {
      return interrupted();
    }
  }
}

/*
 * Wake the threads asleep on f, whose wait bit the caller saw. A
 * sleeper (acquire) holds q.mu from the time it sets the bit until it
 * is in its wait.
 * 1. Clear the wait bit.
 * 2. Take q.mu and let it go at once. A sleeper that set the bit is
 *    then in its wait. One that has yet to take q.mu sets the bit and
 *    tries its pin again, and gets it. Without this step, the notify
 *    can come between a sleeper's failed try and its wait, and be
 *    lost: it sleeps with the bit clear, and no release wakes it.
 * 3. Notify every sleeper, with q.mu let go, so that none wakes only
 *    to wait for q.mu.
 * Each woken sleeper sets the bit again before it tries its pin; the
 * last of them to take q.mu, with the queue's sleepers at 0, clears it
 * (acquire, step 4).
 */
void bufpool_t::wake(frame_t *f) {
  f->state.fetch_and(~state_wait, std::memory_order_relaxed);
  waitq_t &q = waitq_of(f);
  // Step 2: no sleeper between its try and its wait.
  {
    std::lock_guard<std::mutex> guard(q.mu);
  }
  q.cv.notify_all();
}

/* Drop held, state_ex or 1, of f's pin; the last wakes its sleepers. */
void bufpool_t::release(frame_t *f, uint32_t held) {
  uint32_t s = f->state.fetch_sub(held, std::memory_order_release) - held;
  if (state_wait == s) {
    wake(f);
  }
}

/* Unlink f from its chain, by its tag. f is pinned exclusive. */
void bufpool_t::unlink(frame_t *f) {
  size_t b = bucket_of(f->tag());
  int32_t me = static_cast<int32_t>(f - m_frames.get());
  std::lock_guard<std::mutex> guard(m_locks[b % bucket_locks]);
  std::atomic<int32_t> *at = &m_buckets[b];
  for (;;) {
    int32_t i = at->load(std::memory_order_relaxed);
    assert(i >= 0);
    if (i == me) {
      // f keeps its next, so a reader on f walks on.
      at->store(f->next.load(std::memory_order_relaxed),
                std::memory_order_release);
      return;
    }
    at = &m_frames[static_cast<size_t>(i)].next;
  }
}

/*
 * Take a frame by the clock, emptied and pinned exclusive: 0, or -1 if
 * interrupted.
 * 1. Move the hand to the next frame.
 * 2. Skip the frame if it is pinned.
 * 3. If it has an age, shift the age right, by one if its page is
 *    inner and by two if a leaf, and go on to the next frame.
 * 4. Pin a frame of age 0 exclusive. If another thread pinned it
 *    first, go on to the next frame.
 * 5. Empty it: unlink its page from its chain, clear its tag, and let
 *    its shared copy go. Return it.
 * 6. After nine turns of the hand with no frame taken, return if
 *    interrupted; else sleep, and start again at step 1. Every frame
 *    was pinned or aged, but a pin is short and their number bounded.
 */
int bufpool_t::victim(frame_t *&out) {
  for (;;) {
    // Eight turns of the hand age an inner page to nothing, the ninth
    // takes it.
    for (size_t i = 0; i < 9 * m_nframes; i++) {
      uint64_t at = m_hand.fetch_add(1, std::memory_order_relaxed);
      frame_t *f = &m_frames[at % m_nframes];
      if (0 != f->state.load(std::memory_order_relaxed)) {
        continue;
      }
      uint8_t a = f->age.load(std::memory_order_relaxed);
      if (0 != a) {
        int shift = f->inner.load(std::memory_order_relaxed) ? 1 : 2;
        f->age.store(static_cast<uint8_t>(a >> shift),
                     std::memory_order_relaxed);
        continue;
      }
      uint32_t idle = 0;
      if (!f->state.compare_exchange_strong(idle, state_ex,
                                            std::memory_order_acquire,
                                            std::memory_order_relaxed)) {
        continue;
      }
      if (no_page != f->len.load(std::memory_order_relaxed)) {
        unlink(f);
        f->set_tag(tag_t{});
        drop_copy(f);
      }
      f->valid = false;
      out = f;
      return 0;
    }
    if (0 != (sm_pending() & sm_wait_mask())) {
      return interrupted();
    }
    // Every frame pinned or aged: give their holders the processor.
    usleep(victim_sleep_us);
  }
}

/*
 * A frame for tag, pinned exclusive, tagged and in its chain, its
 * bytes not valid: 0; 1 if tag has a frame by now, so the caller looks
 * again; -1 if interrupted.
 */
int bufpool_t::take(const tag_t &tag, frame_t *&out) {
  frame_t *f = nullptr;
  if (0 != victim(f)) {
    return -1;
  }
  size_t b = bucket_of(tag);
  {
    // The look under the lock is the one that is sure: no two frames
    // of one page.
    std::lock_guard<std::mutex> guard(m_locks[b % bucket_locks]);
    int32_t i = m_buckets[b].load(std::memory_order_relaxed);
    for (; i >= 0; i = m_frames[static_cast<size_t>(i)].next.load(
                       std::memory_order_relaxed)) {
      if (m_frames[static_cast<size_t>(i)].tag() == tag) {
        break;
      }
    }
    if (i < 0) {
      f->set_tag(tag);
      f->next.store(m_buckets[b].load(std::memory_order_relaxed),
                    std::memory_order_relaxed);
      m_buckets[b].store(static_cast<int32_t>(f - m_frames.get()),
                         std::memory_order_release);
      out = f;
      return 0;
    }
  }
  release(f, state_ex);
  return 1;
}

/* Pin a page; see bufpool.h. */
int bufpool_t::pin(const bufpool_page_t &page, bool ex, unsigned flags,
                   bufpool_load_fn load, void *arg, frame_t *&out,
                   std::string_view &bytes) {
  bytes = std::string_view();
  if (page.key.size() > bufpool_key_max) {
    return set_error(sm1::SM_ERR_INVALID, "a page's key is too long");
  }
  assert(ex || 0 == (flags & bufpool_no_read));
  tag_t tag(page);
  bool inner = 0 != (flags & bufpool_inner);
  for (;;) {
    frame_t *f = find(tag);
    if (nullptr != f) {
      int rc = acquire(f, ex, tag);
      if (rc < 0) {
        return -1;
      }
      if (1 == rc) {
        continue;
      }
      if (!(f->tag() == tag)) {
        // Taken for another page before the pin.
        release(f, ex ? state_ex : 1);
        continue;
      }
    } else {
      int rc = take(tag, f);
      if (rc < 0) {
        return -1;
      }
      if (1 == rc) {
        continue;
      }
      if (0 == (flags & bufpool_no_read)) {
        // Exclusive through the read: the page's other pins, a writer's
        // among them, wait on the frame, so no write passes this read.
        size_t size = 0;
        rc = load(arg, bytes_of(f), size);
        if (0 != rc) {
          discard(f);
          release(f, state_ex);
          return rc;
        }
        f->size = static_cast<uint16_t>(size);
        f->valid = true;
        if (!ex) {
          // Drop to shared, and let the page's readers in.
          uint32_t s = f->state.load(std::memory_order_relaxed);
          while (!f->state.compare_exchange_weak(s, (s & ~state_ex) + 1,
                                                 std::memory_order_release,
                                                 std::memory_order_relaxed)) {
          }
          if (0 != (s & state_wait)) {
            wake(f);
          }
        }
      }
    }
    touch(f);
    if (f->inner.load(std::memory_order_relaxed) != inner) {
      f->inner.store(inner, std::memory_order_relaxed);
    }
    out = f;
    if (f->valid) {
      bytes = std::string_view(bytes_of(f), f->size);
    }
    return 0;
  }
}

/* Unpin a frame; see bufpool.h. */
void bufpool_t::unpin(frame_t *frame) {
  if (nullptr == frame) {
    return;
  }
  // A holder of the bit is its only pin, so this reads its own.
  bool ex = 0 != (frame->state.load(std::memory_order_relaxed) & state_ex);
  if (ex && !frame->valid &&
      no_page != frame->len.load(std::memory_order_relaxed)) {
    // A new page never installed: its write failed.
    discard(frame);
  }
  release(frame, ex ? state_ex : 1);
}

/* Copy a page's bytes in; see bufpool.h. */
void bufpool_t::install(frame_t *frame, std::string_view bytes) {
  assert(bytes.size() <= bufpool_frame_bytes);
  memcpy(bytes_of(frame), bytes.data(), bytes.size());
  frame->size = static_cast<uint16_t>(bytes.size());
  frame->valid = true;
  frame->version.fetch_add(1, std::memory_order_release);
  drop_copy(frame);
}

/*
 * Let f's shared copy go, its page changed or gone: under an exclusive
 * pin, so no share() of f runs meanwhile. Its holders keep theirs.
 */
void bufpool_t::drop_copy(frame_t *f) {
  if (f->copied.load(std::memory_order_relaxed)) {
    m_copies[static_cast<size_t>(f - m_frames.get())].store(
        nullptr, std::memory_order_release);
    f->copied.store(false, std::memory_order_relaxed);
  }
}

/* The shared copy of a frame's page; see bufpool.h. */
std::shared_ptr<const bufpool_copy_t> bufpool_t::share(frame_t *frame,
                                                       std::string_view bytes) {
  // Under the caller's pin: the version, the bytes and the slot hold
  // still but for another reader's share.
  auto &slot = m_copies[static_cast<size_t>(frame - m_frames.get())];
  uint64_t v = version(frame);
  std::shared_ptr<const bufpool_copy_t> now =
      slot.load(std::memory_order_acquire);
  if (nullptr != now && v == now->version) {
    return now;
  }
  auto made = std::make_shared<const bufpool_copy_t>(
      bufpool_copy_t{v, std::string(bytes)});
  // Another reader, pinned shared as well, may publish first: take its.
  if (slot.compare_exchange_strong(now, made, std::memory_order_acq_rel,
                                   std::memory_order_acquire)) {
    frame->copied.store(true, std::memory_order_relaxed);
    return made;
  }
  return nullptr != now && v == now->version ? now : made;
}

/* Age a frame as used; see bufpool.h. */
void bufpool_t::touch(frame_t *frame) {
  // Any use leaves an age of 128 to 255, eight sweeps for an inner
  // page and four for a leaf, as the lower bits shift out first: no
  // use promotes a page further, so a scan's re-read of a leaf
  // promotes nothing. What is already so is not written, so a hot
  // page's line stays clean of it.
  uint8_t a = frame->age.load(std::memory_order_relaxed);
  if (0 == (a & age_pinned)) {
    frame->age.store(static_cast<uint8_t>(a | age_pinned),
                     std::memory_order_relaxed);
  }
}

/* A frame's version; see bufpool.h. */
uint64_t bufpool_t::version(const frame_t *frame) const {
  return frame->version.load(std::memory_order_acquire);
}

/* Take a frame out; see bufpool.h. */
void bufpool_t::discard(frame_t *frame) {
  unlink(frame);
  frame->set_tag(tag_t{});
  drop_copy(frame);
  frame->valid = false;
  frame->age.store(0, std::memory_order_relaxed);
}

/* Empty a prefix's frames; see bufpool.h. */
void bufpool_t::purge(uint64_t dbid, uint64_t storeid,
                      std::string_view prefix) {
  auto mine = [&](const tag_t &t) {
    if (no_page == t.len || t.dbid != dbid || t.storeid != storeid ||
        t.len < prefix.size()) {
      return false;
    }
    char k[bufpool_key_max];
    memcpy(k, &t.k0, 8);
    memcpy(k + 8, &t.k1, 8);
    return 0 == memcmp(k, prefix.data(), prefix.size());
  };
  for (size_t i = 0; i < m_nframes; i++) {
    frame_t *f = &m_frames[i];
    if (!mine(f->tag())) {
      continue;
    }
    // This wait must not fail, or a dropped page would stay: a pin is
    // short, so it spins with the processor given up.
    while (!try_pin(f->state, true, true)) {
      usleep(victim_sleep_us);
    }
    if (mine(f->tag())) {
      discard(f);
    }
    release(f, state_ex);
  }
}

/* The frames pinned exclusive; see bufpool.h. */
size_t bufpool_t::exclusive() const {
  size_t n = 0;
  for (size_t i = 0; i < m_nframes; i++) {
    uint32_t s = m_frames[i].state.load(std::memory_order_relaxed);
    n += 0 != (s & state_ex) ? 1 : 0;
  }
  return n;
}

/* Whether a page's frame has its wait bit set; see bufpool.h. */
bool bufpool_t::waiting(const bufpool_page_t &page) const {
  const frame_t *f = find(tag_t(page));
  return nullptr != f &&
         0 != (f->state.load(std::memory_order_relaxed) & state_wait);
}

/* A page's age; see bufpool.h. */
uint8_t bufpool_t::age(const bufpool_page_t &page) const {
  const frame_t *f = find(tag_t(page));
  return nullptr == f ? 0 : f->age.load(std::memory_order_relaxed);
}

/* Whether a page has a frame; see bufpool.h. */
bool bufpool_t::cached(const bufpool_page_t &page) const {
  return nullptr != find(tag_t(page));
}
