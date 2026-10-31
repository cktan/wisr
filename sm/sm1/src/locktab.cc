/*
 * locktab.cc -- the lock table and Gray's modes (locktab.h,
 * SM1_DESIGN.md §5.1).
 */

#include "locktab.h"

namespace {

/* A mode as an index into the tables below: IS, IX, S, SIX, X. */
int rank(sm_lockmode_t m) {
  switch (m) {
  case SM_LOCKMODE_IS:
    return 0;
  case SM_LOCKMODE_IX:
    return 1;
  case SM_LOCKMODE_SH:
    return 2;
  case SM_LOCKMODE_SIX:
    return 3;
  case SM_LOCKMODE_EX:
    return 4;
  }
  return 4;
}

} // namespace

namespace wisr {

/* Gray's compatibility of two modes; see locktab.h. */
bool lock_compatible(sm_lockmode_t a, sm_lockmode_t b) {
  static const bool ok[5][5] = {
      /*        IS     IX     S      SIX    X    */
      /* IS  */ {true, true, true, true, false},
      /* IX  */ {true, true, false, false, false},
      /* S   */ {true, false, true, false, false},
      /* SIX */ {true, false, false, false, false},
      /* X   */ {false, false, false, false, false},
  };
  return ok[rank(a)][rank(b)];
}

/* The weakest mode covering both; see locktab.h. */
sm_lockmode_t lock_cover(sm_lockmode_t a, sm_lockmode_t b) {
  static const sm_lockmode_t sup[5][5] = {
      {SM_LOCKMODE_IS, SM_LOCKMODE_IX, SM_LOCKMODE_SH, SM_LOCKMODE_SIX,
       SM_LOCKMODE_EX},
      {SM_LOCKMODE_IX, SM_LOCKMODE_IX, SM_LOCKMODE_SIX, SM_LOCKMODE_SIX,
       SM_LOCKMODE_EX},
      {SM_LOCKMODE_SH, SM_LOCKMODE_SIX, SM_LOCKMODE_SH, SM_LOCKMODE_SIX,
       SM_LOCKMODE_EX},
      {SM_LOCKMODE_SIX, SM_LOCKMODE_SIX, SM_LOCKMODE_SIX, SM_LOCKMODE_SIX,
       SM_LOCKMODE_EX},
      {SM_LOCKMODE_EX, SM_LOCKMODE_EX, SM_LOCKMODE_EX, SM_LOCKMODE_EX,
       SM_LOCKMODE_EX},
  };
  return sup[rank(a)][rank(b)];
}

/* Free every entry, the spares and the arrays. */
locktab_t::part_t::~part_t() {
  for (entry_t **b : {m_buckets, m_old}) {
    size_t n = b == m_buckets ? m_mask + 1 : m_old_mask + 1;
    for (size_t i = 0; nullptr != b && i < n; i++) {
      while (nullptr != b[i]) {
        entry_t *e = b[i];
        b[i] = e->next;
        delete[] e->more;
        delete e;
      }
    }
  }
  while (nullptr != m_free) {
    entry_t *e = m_free;
    m_free = e->next;
    delete[] e->more;
    delete e;
  }
  delete[] m_buckets;
  delete[] m_old;
}

/* The link to id's entry, in either array, or nullptr. */
locktab_t::entry_t **locktab_t::part_t::link_of(const lockid_t &id, size_t h) {
  // The new array, then the one a resize moves from: a bucket moved
  // is empty there.
  for (entry_t **b : {m_buckets, m_old}) {
    if (nullptr == b) {
      continue;
    }
    entry_t **at = &b[h & (b == m_buckets ? m_mask : m_old_mask)];
    for (; nullptr != *at; at = &(*at)->next) {
      if ((*at)->hash == h && (*at)->id == id) {
        return at;
      }
    }
  }
  return nullptr;
}

/* The entry of id, or nullptr; see locktab.h. */
locktab_t::entry_t *locktab_t::part_t::find(const lockid_t &id) {
  entry_t **at = link_of(id, lockid_hash_t{}(id));
  return nullptr != at ? *at : nullptr;
}

/* owner's hold in e, or nullptr; see locktab.h. */
locktab_t::holder_t *locktab_t::held(entry_t *e, const void *owner) {
  for (uint32_t i = 0; i < e->n; i++) {
    if (e->at(i).owner == owner) {
      return &e->at(i);
    }
  }
  return nullptr;
}

namespace {

/* The modes that conflict with each mode, as bits by their values. */
struct conflict_masks_t {
  uint32_t of[5] = {};

  /* Read them off Gray's table, once. */
  conflict_masks_t() {
    for (int want = 0; want < 5; want++) {
      for (int held = 0; held < 5; held++) {
        if (!wisr::lock_compatible(static_cast<sm_lockmode_t>(held),
                                   static_cast<sm_lockmode_t>(want))) {
          of[want] |= uint32_t(1) << held;
        }
      }
    }
  }
};

/* The modes, as bits by their values, that conflict with mode m. */
uint32_t conflict_mask(sm_lockmode_t m) {
  static const conflict_masks_t masks;
  return masks.of[m];
}

/* Append h to e's holders, growing its array past the inline ones. */
void push_holder(locktab_t::entry_t *e, const locktab_t::holder_t &h) {
  using entry_t = locktab_t::entry_t;
  if (e->n >= entry_t::inline_holders &&
      e->n - entry_t::inline_holders == e->cap) {
    // Twice the room, the holders kept in their order.
    uint32_t cap = 0 == e->cap ? 4 : 2 * e->cap;
    locktab_t::holder_t *more = new locktab_t::holder_t[cap];
    for (uint32_t i = 0; i < e->cap; i++) {
      more[i] = e->more[i];
    }
    delete[] e->more;
    e->more = more;
    e->cap = cap;
  }
  e->at(e->n++) = h;
}

/* Remove e's holder i, the later ones moving up, still in order. */
void erase_holder(locktab_t::entry_t *e, uint32_t i) {
  for (; i + 1 < e->n; i++) {
    e->at(i) = e->at(i + 1);
  }
  e->n--;
}

} // namespace

/* One walk of e's holders for owner asking mode; see locktab.h. */
bool locktab_t::probe(entry_t *e, const void *owner, sm_lockmode_t mode,
                      holder_t **mine, sm_lockmode_t *want) {
  *mine = nullptr;
  // Our own hold, and the others' modes as bits by their values.
  uint32_t others = 0;
  for (uint32_t i = 0; i < e->n; i++) {
    holder_t &h = e->at(i);
    if (h.owner == owner) {
      *mine = &h;
    } else {
      others |= uint32_t(1) << h.mode;
    }
  }
  *want = nullptr != *mine ? lock_cover((*mine)->mode, mode) : mode;
  return 0 != (others & conflict_mask(*want));
}

/* An entry for id, from the free list or new. */
locktab_t::entry_t *locktab_t::part_t::take_entry(const lockid_t &id,
                                                  size_t h) {
  entry_t *e = m_free;
  if (nullptr != e) {
    m_free = e->next;
    m_nfree--;
  } else {
    e = new entry_t{};
  }
  e->id = id;
  e->hash = h;
  e->n = 0;
  return e;
}

/* Give e back: to the free list, its holders' array with it, or freed. */
void locktab_t::part_t::give_entry(entry_t *e) {
  if (m_nfree < free_max) {
    e->next = m_free;
    m_free = e;
    m_nfree++;
    return;
  }
  delete[] e->more;
  delete e;
}

/* Begin moving every bucket into a new array of n. */
void locktab_t::part_t::resize(size_t n) {
  m_old = m_buckets;
  m_old_mask = m_mask;
  m_moved = 0;
  m_buckets = new entry_t *[n]();
  m_mask = n - 1;
}

/* Grow or shrink if the load says to, then move a few buckets. */
void locktab_t::part_t::rebalance() {
  if (nullptr == m_old) {
    size_t n = m_mask + 1;
    if (m_size > n) {
      resize(2 * n);
    } else if (n > min_buckets && m_size < n / 8) {
      // A quarter: half full at most after, and one resize, not two.
      resize(n / 4 > min_buckets ? n / 4 : min_buckets);
    } else {
      return;
    }
  }
  // Move the next few buckets that hold entries, each entry by the
  // hash it keeps; an empty one costs a look, so more of those.
  size_t moved = 0;
  for (size_t looked = 0;
       moved < move_step && looked < 8 * move_step && m_moved <= m_old_mask;
       looked++, m_moved++) {
    if (nullptr == m_old[m_moved]) {
      continue;
    }
    moved++;
    while (nullptr != m_old[m_moved]) {
      entry_t *e = m_old[m_moved];
      m_old[m_moved] = e->next;
      entry_t **to = &m_buckets[e->hash & m_mask];
      e->next = *to;
      *to = e;
    }
  }
  // All moved: the old array goes.
  if (m_moved > m_old_mask) {
    delete[] m_old;
    m_old = nullptr;
  }
}

/* A new hold of owner's on id; see locktab.h. */
void locktab_t::part_t::add(entry_t *e, const lockid_t &id, void *owner,
                            sm_lockmode_t mode, void *first) {
  if (nullptr == e) {
    if (nullptr == m_buckets) {
      m_buckets = new entry_t *[min_buckets]();
      m_mask = min_buckets - 1;
    }
    // A new entry, at the head of its bucket in the new array.
    size_t h = lockid_hash_t{}(id);
    e = take_entry(id, h);
    entry_t **to = &m_buckets[h & m_mask];
    e->next = *to;
    *to = e;
    m_size++;
    push_holder(e, {owner, mode, first});
    rebalance();
    return;
  }
  push_holder(e, {owner, mode, first});
}

/* owner's hold on id covers mode; see locktab.h. */
void locktab_t::part_t::join(const lockid_t &id, void *owner,
                             sm_lockmode_t mode, void *first) {
  entry_t *e = find(id);
  holder_t *h = nullptr != e ? held(e, owner) : nullptr;
  if (nullptr != h) {
    h->mode = lock_cover(h->mode, mode);
  } else {
    add(e, id, owner, mode, first);
  }
}

/* Drop owner's hold on id if first made it; see locktab.h. */
void locktab_t::part_t::drop(const lockid_t &id, const void *owner,
                             const void *first) {
  entry_t **at = link_of(id, lockid_hash_t{}(id));
  // Gone already: a rollback may have released it.
  if (nullptr == at) {
    return;
  }
  entry_t *e = *at;
  // Drop owner from the holder list with the hold's first entry; a
  // later one is released alone.
  for (uint32_t i = 0; i < e->n; i++) {
    if (e->at(i).owner == owner) {
      if (first == e->at(i).first) {
        erase_holder(e, i);
      }
      break;
    }
  }
  // The last holder out removes the entry.
  if (0 == e->n) {
    *at = e->next;
    give_entry(e);
    m_size--;
    rebalance();
  }
}

/* to holds what from held on id; see locktab.h. */
void locktab_t::part_t::move_owner(const lockid_t &id, const void *from,
                                   void *to) {
  entry_t *e = find(id);
  if (nullptr == e) {
    return;
  }
  for (uint32_t i = 0; i < e->n; i++) {
    if (e->at(i).owner == from) {
      e->at(i).owner = to;
    }
  }
}

} // namespace wisr
