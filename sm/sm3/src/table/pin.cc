/*
 * pin.cc -- the session's pool of pins (../ROWSTORE.md §6.4), moved
 * from sm1 with its rules: a pin ref names a pin and the generation
 * it was taken at; releasing the pin increases its generation, so
 * every ref to it goes stale, and the pin goes back to the pool for
 * the next read. A cursor's pin is moved on, not released, at each
 * row, and deforms its row as far as its caller reads.
 */

#include "pin.h"

namespace {

/* Link pin at the head of the list head. */
void link(sm3::pin_t *&head, sm3::pin_t *p) {
  p->prev = nullptr;
  p->next = head;
  if (head) {
    head->prev = p;
  }
  head = p;
}

/* Unlink pin from the list head. */
void unlink(sm3::pin_t *&head, sm3::pin_t *p) {
  if (p->prev) {
    p->prev->next = p->next;
  } else {
    head = p->next;
  }
  if (p->next) {
    p->next->prev = p->prev;
  }
  p->prev = p->next = nullptr;
}

/* Whether ref names a pin still holding the row it was taken for. */
bool live(const sm3::pin_ref_t &ref) {
  return ref.pin && ref.gen == ref.pin->gen;
}

/* Fail with SM_ERR_INVALID: ref names no row. */
int stale() {
  return sm1::seterr(sm1::SM_ERR_INVALID, "pin: a stale ref names no row");
}

/* Let go of what a pin holds, and make every ref to it stale. */
void drop(sm3::pin_t *p) {
  // Let go of the iterator, and with it the blocks and the
  // SuperVersion it holds.
  sm1::unpin(p->raw);
  p->row = {};
  p->reader = {};
  p->gen++;
}

} // namespace

namespace sm3 {

/* A pin from the pool, free or new; see pin.h. */
pin_t *pin_take(session_t *s) {
  // Reuse a released pin when there is one.
  if (pin_t *p = s->pins_free) {
    unlink(s->pins_free, p);
    return p;
  }
  // Else make one; the pool keeps it until the session closes.
  pin_t *p = new pin_t;
  p->raw = sm1::rawpin_new();
  p->session = s;
  s->pins.push_back(p);
  return p;
}

/* Take back the pin ref holds, to read another row into; see pin.h. */
pin_t *pin_reread(pin_ref_t &ref) {
  pin_t *p = live(ref) && !ref.pin->scan ? ref.pin : nullptr;
  ref = pin_ref_t{};
  if (!p) {
    return nullptr;
  }
  unlink(p->session->pins_held, p);
  // The raw pin keeps the row's iterator; sm1's read replaces the row.
  p->row = {};
  p->reader = {};
  p->gen++;
  return p;
}

/* Hold row in a taken pin, and point ref at it; see pin.h. */
void pin_hold(pin_t *p, const std::shared_ptr<const table_def_t> &def,
              std::string_view row, pin_ref_t &ref) {
  // A definition is replaced, never changed: share it only when it
  // is another.
  if (p->def != def) {
    p->def = def;
  }
  p->row = row;
  p->reader = {};
  link(p->session->pins_held, p);
  ref = pin_ref_t{p, p->gen};
}

/* Give back a taken pin; see pin.h. */
void pin_put(pin_t *p) {
  // No ref was given out for it, but drop what it may hold.
  sm1::unpin(p->raw);
  p->row = {};
  link(p->session->pins_free, p);
}

/* Make a taken pin a cursor's; see pin.h. */
void pin_scan_hold(pin_t *p, scan_t *scan,
                   const std::shared_ptr<const table_def_t> &def) {
  p->def = def;
  p->scan = scan;
  link(p->session->pins_scan, p);
}

/* Point a cursor's pin at its next row; see pin.h. */
void pin_scan_move(pin_t *p, std::string_view row) {
  // The cursor's iterator holds the bytes; the pin only views them.
  p->row = row;
  p->reader = {};
  p->gen++;
}

/* Release a pin to the pool; see pin.h. */
void pin_release(pin_t *p) {
  drop(p);
  unlink(p->scan ? p->session->pins_scan : p->session->pins_held, p);
  p->scan = nullptr;
  link(p->session->pins_free, p);
}

/* Make the pins of the cursors a rollback closed stale; see pin.h. */
void pins_rolled_back(session_t *s) {
  for (pin_t *p = s->pins_scan; p; p = p->next) {
    // A tree's cursor has no rel scan for the rollback to close: its
    // rows are in its own raw pins, each batch read at the view of
    // its call, so it goes on (../ROWSTORE.md §7.2).
    if (p->scan->rel && sm2::rel_scan_rolled_back(p->scan->rel)) {
      drop(p);
    }
  }
}

/* Free every pin of the pool; see pin.h. */
void pins_close(session_t *s) {
  for (pin_t *p : s->pins) {
    // Emptied first: no iterator outlives the database.
    sm1::rawpin_free(p->raw);
    delete p;
  }
  s->pins.clear();
  s->pins_free = s->pins_held = s->pins_scan = nullptr;
  // And the raw pins tree cursors gave back, emptied already.
  for (sm1::rawpin_t *raw : s->raws_free) {
    sm1::rawpin_free(raw);
  }
  s->raws_free.clear();
}

/* Release the pin ref names; see sm3.h. */
void unpin(pin_ref_t &ref) {
  // A cursor's pin is its cursor's to release.
  if (live(ref) && !ref.pin->scan) {
    pin_release(ref.pin);
  }
  ref = pin_ref_t{};
}

/* Release every pin s holds; see sm3.h. */
void release_all_pins(session_t *s) {
  if (!s) {
    return;
  }
  while (s->pins_held) {
    pin_release(s->pins_held);
  }
  // A cursor's row goes with its transaction; the pin stays its.
  for (pin_t *p = s->pins_scan; p; p = p->next) {
    drop(p);
  }
}

/* Attributes from to n - 1 of the row ref pins; see sm3.h. */
int pin_deform(pin_ref_t ref, size_t from, size_t n,
               std::span<sm2::datum_t> datums, std::span<bool> isnull) {
  if (!live(ref)) {
    return stale();
  }
  pin_t *p = ref.pin;
  sm2::tup_reader_t &r = p->reader;
  // Begun on the first deform, so a row nobody deforms is never
  // parsed; begun again for a caller that is behind it.
  if (!r.desc || from < r.next) {
    if (sm2::tup_read_begin(p->def->desc, p->row, r) != 0) {
      return -1;
    }
  }
  return sm2::tup_read(r, n, datums, isnull);
}

/* One attribute of the row ref pins; see sm3.h. */
int pin_decode(pin_ref_t ref, size_t ordinal, sm2::datum_t &val, bool &isnull) {
  if (!live(ref)) {
    return stale();
  }
  return sm2::tup_decode(ref.pin->def->desc, ref.pin->row, ordinal, val,
                         isnull);
}

/* The bytes of the row ref pins; see sm3.h. */
int pin_tuple(pin_ref_t ref, std::string_view &tuple) {
  if (!live(ref)) {
    tuple = {};
    return stale();
  }
  tuple = ref.pin->row;
  return 0;
}

} // namespace sm3
