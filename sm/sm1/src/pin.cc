/*
 * pin.cc -- the session's pool of pins (../PIN.md §4): a pin
 * keeps the iterator that found a row, so the row is handed out
 * without a copy, until the pin is released. A pin ref names a
 * pin and the generation it was taken at; releasing the pin
 * increases its generation, so every ref to it goes stale, and
 * the pin goes back to the pool for the next read.
 */

#include "session.h"

#include <memory>

using namespace wisr;

namespace {

/* Link pin at the head of the session's held list. */
void link_held(sm1::pin_t *p) {
  sm1::session_t *s = p->session;
  p->prev = nullptr;
  p->next = s->pins_held;
  if (s->pins_held) {
    s->pins_held->prev = p;
  }
  s->pins_held = p;
}

/* Unlink pin from the session's held list. */
void unlink_held(sm1::pin_t *p) {
  sm1::session_t *s = p->session;
  if (p->prev) {
    p->prev->next = p->next;
  } else {
    s->pins_held = p->next;
  }
  if (p->next) {
    p->next->prev = p->prev;
  }
  p->prev = p->next = nullptr;
}

/* Push pin onto the session's free list. */
void push_free(sm1::pin_t *p) {
  sm1::session_t *s = p->session;
  p->prev = nullptr;
  p->next = s->pins_free;
  s->pins_free = p;
}

} // namespace

namespace wisr {

/* Take a free pin, or make one; it is on neither list. */
sm1::pin_t *pin_take(sm1::session_t *s) {
  // Reuse a released pin when there is one.
  if (sm1::pin_t *p = s->pins_free) {
    s->pins_free = p->next;
    p->next = nullptr;
    return p;
  }
  // Else make one; the pool keeps it until the session closes.
  s->pins.push_back(std::make_unique<sm1::pin_t>());
  sm1::pin_t *p = s->pins.back().get();
  p->session = s;
  return p;
}

/* Hold the row a taken pin found, and point ref at it. */
void pin_hold(sm1::pin_t *p, sm1::pin_ref_t &ref) {
  link_held(p);
  ref = sm1::pin_ref_t{p, p->gen};
}

/* Give a taken pin that found no row back to the pool. */
void pin_put(sm1::pin_t *p) {
  // No ref was given out for it, but drop what it may hold.
  p->it.reset();
  p->row = rocksdb::Slice();
  push_free(p);
}

/* Whether ref names a pin that is still held. */
bool pin_live(const sm1::pin_ref_t &ref) {
  return ref.pin && ref.gen == ref.pin->gen;
}

/* Release a held pin, making every ref to it stale. */
void pin_release(sm1::pin_t *p) {
  // Let go of the iterator, and with it the blocks and the
  // SuperVersion it holds.
  p->it.reset();
  p->row = rocksdb::Slice();
  // Every ref taken on it no longer matches.
  p->gen++;
  unlink_held(p);
  push_free(p);
}

/* Release every pin the session holds. */
void pin_release_all(sm1::session_t *s) {
  while (s->pins_held) {
    pin_release(s->pins_held);
  }
}

} // namespace wisr

/* Release the pin ref names, if it is still held. */
void sm1::unpin(sm1::pin_ref_t &ref) {
  if (pin_live(ref)) {
    pin_release(ref.pin);
  }
  ref = sm1::pin_ref_t{};
}

/* Release every pin the session holds; its transaction stays open. */
void sm1::release_all_pins(sm1::session_t *session) {
  if (session) {
    pin_release_all(session);
  }
}
