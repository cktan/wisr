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

/* The entry of id, or nullptr; see locktab.h. */
locktab_t::entry_t *locktab_t::part_t::find(const lockid_t &id) {
  auto it = m_map.find(id);
  return it != m_map.end() ? &it->second : nullptr;
}

/* owner's hold in e, or nullptr; see locktab.h. */
locktab_t::holder_t *locktab_t::held(entry_t *e, const void *owner) {
  for (holder_t &h : e->holders) {
    if (h.owner == owner) {
      return &h;
    }
  }
  return nullptr;
}

/* A new hold of owner's on id; see locktab.h. */
void locktab_t::part_t::add(entry_t *e, const lockid_t &id, void *owner,
                            sm_lockmode_t mode, void *first) {
  // Join the holders, or make the entry: one lookup either way.
  entry_t &to = nullptr != e ? *e : m_map[id];
  to.holders.push_back({owner, mode, first});
}

/* owner's hold on id covers mode; see locktab.h. */
void locktab_t::part_t::join(const lockid_t &id, void *owner,
                             sm_lockmode_t mode, void *first) {
  entry_t &e = m_map[id];
  holder_t *h = held(&e, owner);
  if (nullptr != h) {
    h->mode = lock_cover(h->mode, mode);
  } else {
    e.holders.push_back({owner, mode, first});
  }
}

/* Drop owner's hold on id if first made it; see locktab.h. */
void locktab_t::part_t::drop(const lockid_t &id, const void *owner,
                             const void *first) {
  auto it = m_map.find(id);
  // Gone already: a rollback may have released it.
  if (it == m_map.end()) {
    return;
  }
  std::vector<holder_t> &holders = it->second.holders;
  // Drop owner from the holder list with the hold's first entry; a
  // later one is released alone.
  for (size_t i = 0; i < holders.size(); i++) {
    if (holders[i].owner == owner) {
      if (first == holders[i].first) {
        holders.erase(holders.begin() + i);
      }
      break;
    }
  }
  // The last holder out removes the entry.
  if (holders.empty()) {
    m_map.erase(it);
  }
}

/* to holds what from held on id; see locktab.h. */
void locktab_t::part_t::move_owner(const lockid_t &id, const void *from,
                                   void *to) {
  entry_t *e = find(id);
  if (nullptr == e) {
    return;
  }
  for (holder_t &h : e->holders) {
    if (h.owner == from) {
      h.owner = to;
    }
  }
}

} // namespace wisr
