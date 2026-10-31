/*
 * test_locktab.cc -- the lock table alone (src/locktab.h): ids,
 * Gray's modes and opaque owners, with no session, wait or
 * database. test_lock.cc tests the lock manager on top of it
 * through sm1.h.
 */

#include "doctest.h"

#include "locktab.h"

#include <algorithm>
#include <vector>

using wisr::lock_compatible;
using wisr::lock_cover;
using wisr::lockid_t;
using wisr::locktab_t;

namespace {

/* The modes in the order of Gray's table: IS, IX, S, SIX, X. */
const sm_lockmode_t g_modes[] = {SM_LOCKMODE_IS, SM_LOCKMODE_IX, SM_LOCKMODE_SH,
                                 SM_LOCKMODE_SIX, SM_LOCKMODE_EX};

/* The owners of e's holds that conflict with mode for owner, sorted. */
std::vector<void *> conflicts(const locktab_t::entry_t *e, const void *owner,
                              sm_lockmode_t mode) {
  std::vector<void *> out;
  locktab_t::each_conflict(e, owner, mode, [&](void *o) { out.push_back(o); });
  std::sort(out.begin(), out.end());
  return out;
}

/* The count of holds in t. */
size_t holds(const locktab_t &t) {
  size_t n = 0;
  t.each([&](const lockid_t &, const locktab_t::holder_t &) { n++; });
  return n;
}

} // namespace

TEST_CASE("locktab: Gray's modes conflict and convert as his table says") {
  // Verify the compatibility and cover tables, entry by entry.
  // 1. Check compatibility against Gray's table, both ways round.
  // 2. Check that a mode covers itself and X covers all.
  // 3. Check the joins that are not one of the two modes: IX and S
  //    meet at SIX.
  const bool ok[5][5] = {
      {true, true, true, true, false},     {true, true, false, false, false},
      {true, false, true, false, false},   {true, false, false, false, false},
      {false, false, false, false, false},
  };
  for (int i = 0; i < 5; i++) {
    for (int j = 0; j < 5; j++) {
      CHECK(lock_compatible(g_modes[i], g_modes[j]) == ok[i][j]);
      CHECK(lock_compatible(g_modes[j], g_modes[i]) == ok[i][j]);
    }
  }
  for (sm_lockmode_t m : g_modes) {
    CHECK(lock_cover(m, m) == m);
    CHECK(lock_cover(m, SM_LOCKMODE_EX) == SM_LOCKMODE_EX);
    CHECK(lock_cover(SM_LOCKMODE_IS, m) == m);
  }
  CHECK(lock_cover(SM_LOCKMODE_IX, SM_LOCKMODE_SH) == SM_LOCKMODE_SIX);
  CHECK(lock_cover(SM_LOCKMODE_SH, SM_LOCKMODE_IX) == SM_LOCKMODE_SIX);
  CHECK(lock_cover(SM_LOCKMODE_SIX, SM_LOCKMODE_SH) == SM_LOCKMODE_SIX);
}

TEST_CASE("locktab: an entry is made by its first hold and goes with its "
          "last") {
  // Verify add, find, held and drop over two owners of one id.
  // 1. Find nothing in an empty table.
  // 2. Add a's hold; the entry is found, and a holds it.
  // 3. Add b's hold to that entry; both are held, one entry.
  // 4. Drop a's hold; b's stays, and the entry with it.
  // 5. Drop b's hold; the entry is gone.
  locktab_t t;
  int a = 0, b = 0, fa = 0, fb = 0;
  const lockid_t id{1, 42};
  CHECK(nullptr == t.find(id));
  CHECK(t.empty());

  t.add(nullptr, id, &a, SM_LOCKMODE_SH, &fa);
  locktab_t::entry_t *e = t.find(id);
  REQUIRE(nullptr != e);
  REQUIRE(nullptr != locktab_t::held(e, &a));
  CHECK(locktab_t::held(e, &a)->mode == SM_LOCKMODE_SH);
  CHECK(locktab_t::held(e, &a)->first == &fa);
  CHECK(nullptr == locktab_t::held(e, &b));

  t.add(e, id, &b, SM_LOCKMODE_SH, &fb);
  CHECK(1 == t.size());
  CHECK(2 == holds(t));

  t.drop(id, &a, &fa);
  e = t.find(id);
  REQUIRE(nullptr != e);
  CHECK(nullptr == locktab_t::held(e, &a));
  CHECK(nullptr != locktab_t::held(e, &b));
  CHECK(locktab_t::oldest(e).owner == &b);

  t.drop(id, &b, &fb);
  CHECK(nullptr == t.find(id));
  CHECK(t.empty());
}

TEST_CASE("locktab: a hold goes only with the entry that made it") {
  // Verify that drop leaves a hold made by another first, as an OID
  // lock's slot moved into a hold is released alone (fp_move).
  // 1. Add a's hold, made with first f.
  // 2. Join a second entry g of a's on the id, in a stronger mode.
  // 3. Drop with g: the hold stays, in the covering mode.
  // 4. Drop with f: the hold and the entry go.
  // 5. Drop again, and drop an id never held: nothing happens.
  locktab_t t;
  int a = 0, f = 0, g = 0;
  const lockid_t id{1, 7};
  t.add(nullptr, id, &a, SM_LOCKMODE_IS, &f);
  t.join(id, &a, SM_LOCKMODE_SH, &g);
  locktab_t::entry_t *e = t.find(id);
  REQUIRE(nullptr != e);
  CHECK(1 == holds(t));
  CHECK(locktab_t::held(e, &a)->mode == SM_LOCKMODE_SH);
  CHECK(locktab_t::held(e, &a)->first == &f);

  t.drop(id, &a, &g);
  e = t.find(id);
  REQUIRE(nullptr != e);
  CHECK(locktab_t::held(e, &a)->mode == SM_LOCKMODE_SH);

  t.drop(id, &a, &f);
  CHECK(t.empty());
  t.drop(id, &a, &f);
  t.drop({2, 7}, &a, &f);
  CHECK(t.empty());
}

TEST_CASE("locktab: join makes the entry, or covers the owner's hold") {
  // Verify join on an id held by none, and by another owner.
  // 1. Join a's IX on a new id: the entry is made, a holds IX.
  // 2. Join b's IS: b holds IS beside a.
  // 3. Join a's S: a's hold covers both, SIX, made with its first.
  locktab_t t;
  int a = 0, b = 0, fa = 0, fb = 0, fa2 = 0;
  const lockid_t id{0x8000000000000000ULL, 9};
  t.join(id, &a, SM_LOCKMODE_IX, &fa);
  t.join(id, &b, SM_LOCKMODE_IS, &fb);
  t.join(id, &a, SM_LOCKMODE_SH, &fa2);
  locktab_t::entry_t *e = t.find(id);
  REQUIRE(nullptr != e);
  CHECK(2 == holds(t));
  CHECK(locktab_t::held(e, &a)->mode == SM_LOCKMODE_SIX);
  CHECK(locktab_t::held(e, &a)->first == &fa);
  CHECK(locktab_t::held(e, &b)->mode == SM_LOCKMODE_IS);
}

TEST_CASE("locktab: the conflicts of a mode are the other owners' holds it "
          "meets") {
  // Verify each_conflict over three holders of one id.
  // 1. a holds IS, b IX, c IS.
  // 2. For d asking S: b alone conflicts.
  // 3. For b asking X: a and c conflict, b's own hold not.
  // 4. For d asking IS: none conflicts.
  locktab_t t;
  int a = 0, b = 0, c = 0, d = 0;
  const lockid_t id{3, 3};
  t.add(nullptr, id, &a, SM_LOCKMODE_IS, &a);
  locktab_t::entry_t *e = t.find(id);
  t.add(e, id, &b, SM_LOCKMODE_IX, &b);
  t.add(e, id, &c, SM_LOCKMODE_IS, &c);

  CHECK(conflicts(e, &d, SM_LOCKMODE_SH) == std::vector<void *>{&b});
  std::vector<void *> ac = {&a, &c};
  std::sort(ac.begin(), ac.end());
  CHECK(conflicts(e, &b, SM_LOCKMODE_EX) == ac);
  CHECK(conflicts(e, &d, SM_LOCKMODE_IS).empty());
}

TEST_CASE("locktab: an owner's holds move to another in its place") {
  // Verify move_owner, as a prepared transaction's locks go to the
  // session that keeps it.
  // 1. a holds two ids, b one of them.
  // 2. Move a's holds to g on both ids.
  // 3. g holds them in a's modes, made with a's firsts; b is kept.
  // 4. Moving on an id never held does nothing.
  locktab_t t;
  int a = 0, b = 0, g = 0, f1 = 0, f2 = 0, fb = 0;
  const lockid_t one{1, 1}, two{1, 2};
  t.add(nullptr, one, &a, SM_LOCKMODE_SH, &f1);
  t.add(nullptr, two, &a, SM_LOCKMODE_EX, &f2);
  t.add(t.find(one), one, &b, SM_LOCKMODE_SH, &fb);

  t.move_owner(one, &a, &g);
  t.move_owner(two, &a, &g);
  t.move_owner({9, 9}, &a, &g);
  CHECK(nullptr == locktab_t::held(t.find(one), &a));
  CHECK(nullptr == locktab_t::held(t.find(two), &a));
  CHECK(locktab_t::held(t.find(one), &g)->mode == SM_LOCKMODE_SH);
  CHECK(locktab_t::held(t.find(one), &g)->first == &f1);
  CHECK(locktab_t::held(t.find(two), &g)->mode == SM_LOCKMODE_EX);
  CHECK(locktab_t::held(t.find(two), &g)->first == &f2);
  CHECK(nullptr != locktab_t::held(t.find(one), &b));
  CHECK(2 == t.size());
}

TEST_CASE("locktab: an id's hash spreads both its words to its low bits") {
  // Verify the hash's low bits, which the fast path's counts are
  // taken from (modulo 1,024), depend on every bit of the id.
  // 1. Hash OID locks 1,024 oids apart: pairs sharing a count are
  //    as rare as chance makes them, not every pair.
  // 2. Hash a row lock and an OID lock of the same words, which
  //    differ only in hi's top bit: the same.
  wisr::lockid_hash_t h;
  const uint64_t oid = uint64_t(1) << 63;
  int apart = 0, typed = 0;
  for (uint64_t k = 0; k < 1024; k++) {
    if (h({oid, k}) % 1024 == h({oid, k + 1024}) % 1024) {
      apart++;
    }
    if (h({7, k}) % 1024 == h({7 | oid, k}) % 1024) {
      typed++;
    }
  }
  CHECK(apart < 16);
  CHECK(typed < 16);
}
