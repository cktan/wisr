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
size_t holds(const locktab_t::part_t &t) {
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
  locktab_t::part_t t;
  int a = 0, b = 0, fa = 0, fb = 0;
  const lockid_t id{1, 42};
  CHECK(nullptr == t.find(id));
  CHECK(0 == t.size());

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
  CHECK(0 == t.size());
}

TEST_CASE("locktab: a hold goes only with the entry that made it") {
  // Verify that drop leaves a hold made by another first, as an OID
  // lock's slot moved into a hold is released alone (fp_move).
  // 1. Add a's hold, made with first f.
  // 2. Join a second entry g of a's on the id, in a stronger mode.
  // 3. Drop with g: the hold stays, in the covering mode.
  // 4. Drop with f: the hold and the entry go.
  // 5. Drop again, and drop an id never held: nothing happens.
  locktab_t::part_t t;
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
  CHECK(0 == t.size());
  t.drop(id, &a, &f);
  t.drop({2, 7}, &a, &f);
  CHECK(0 == t.size());
}

TEST_CASE("locktab: join makes the entry, or covers the owner's hold") {
  // Verify join on an id held by none, and by another owner.
  // 1. Join a's IX on a new id: the entry is made, a holds IX.
  // 2. Join b's IS: b holds IS beside a.
  // 3. Join a's S: a's hold covers both, SIX, made with its first.
  locktab_t::part_t t;
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
  locktab_t::part_t t;
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
  locktab_t::part_t t;
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

TEST_CASE("locktab: ids spread over the partitions, and the table is all "
          "of them") {
  // Verify an id's partition is fixed by its hash, every partition
  // gets ids, and the whole table's size and holds sum them.
  // 1. Place 1,024 row ids and 1,024 OID ids: each partition is in
  //    range, the same for the same id, and none is left empty.
  // 2. Hold each id in its partition: the table's size is 2,048,
  //    each() visits 2,048 holds, and a partition's size is its
  //    share.
  // 3. Drop them all: the table is empty.
  locktab_t t;
  int a = 0;
  std::vector<lockid_t> ids;
  for (uint64_t k = 0; k < 1024; k++) {
    ids.push_back({5, k});
    ids.push_back({uint64_t(1) << 63, k});
  }
  size_t per[locktab_t::parts] = {};
  for (const lockid_t &id : ids) {
    size_t i = locktab_t::part_of(id);
    REQUIRE(i < locktab_t::parts);
    CHECK(i == locktab_t::part_of(id));
    per[i]++;
    t.part(id).add(nullptr, id, &a, SM_LOCKMODE_SH, &a);
  }
  size_t sum = 0;
  for (size_t i = 0; i < locktab_t::parts; i++) {
    CHECK(per[i] > 0);
    CHECK(t.part(i).size() == per[i]);
    sum += per[i];
  }
  CHECK(sum == ids.size());
  CHECK(t.size() == ids.size());
  size_t n = 0;
  t.each([&](const lockid_t &, const locktab_t::holder_t &) { n++; });
  CHECK(n == ids.size());
  for (const lockid_t &id : ids) {
    t.part(id).drop(id, &a, &a);
  }
  CHECK(t.empty());
}

TEST_CASE("locktab: a partition grows and shrinks a few buckets at a time, "
          "every id found throughout") {
  // Verify the bucket array resizes by moving buckets a call at a
  // time, and no id is lost to a lookup while it does.
  // 1. Add 20,000 ids to one partition, checking every id held so
  //    far each time a resize is under way after a thousand.
  // 2. The array has grown past the ids, and a resize was seen.
  // 3. Drop them all, checking those left the same way; the array
  //    shrinks back towards its least, and the partition is empty.
  locktab_t::part_t t;
  int a = 0;
  const uint64_t n = 20000;
  bool saw = false;
  auto all_found = [&](uint64_t from, uint64_t to) {
    for (uint64_t k = from; k < to; k++) {
      if (nullptr == t.find({9, k})) {
        return false;
      }
    }
    return true;
  };
  for (uint64_t k = 0; k < n; k++) {
    t.add(nullptr, {9, k}, &a, SM_LOCKMODE_SH, &a);
    if (t.resizing() && 0 == k % 1000) {
      saw = true;
      REQUIRE(all_found(0, k + 1));
    }
  }
  CHECK(saw);
  CHECK(t.size() == n);
  CHECK(t.buckets() >= n / 2);
  CHECK(all_found(0, n));
  size_t most = t.buckets();
  saw = false;
  for (uint64_t k = 0; k < n; k++) {
    t.drop({9, k}, &a, &a);
    if (t.resizing() && 0 == k % 1000) {
      saw = true;
      REQUIRE(all_found(k + 1, n));
    }
  }
  CHECK(saw);
  CHECK(0 == t.size());
  CHECK(t.buckets() < most / 16);
}

TEST_CASE("locktab: a partition keeps at most free_max dropped entries") {
  // Verify a dropped entry is kept for reuse up to free_max, and
  // the rest are freed, so a large transaction leaves little behind.
  // 1. Add and drop 3,000 ids: free_max entries are kept.
  // 2. Add 10 ids: they come from the kept ones.
  locktab_t::part_t t;
  int a = 0;
  for (uint64_t k = 0; k < 3000; k++) {
    t.add(nullptr, {9, k}, &a, SM_LOCKMODE_SH, &a);
  }
  for (uint64_t k = 0; k < 3000; k++) {
    t.drop({9, k}, &a, &a);
  }
  CHECK(t.spares() == locktab_t::part_t::free_max);
  for (uint64_t k = 0; k < 10; k++) {
    t.add(nullptr, {9, k}, &a, SM_LOCKMODE_SH, &a);
  }
  CHECK(t.spares() == locktab_t::part_t::free_max - 10);
  CHECK(10 == t.size());
}

TEST_CASE("locktab: holders past the inline ones keep their order") {
  // Verify an entry's holders beyond its inline three are kept in
  // the order they came, through drops and an entry's reuse.
  // 1. Ten owners hold one id S; the oldest is the first.
  // 2. X conflicts with all ten, S with none.
  // 3. Drop the oldest and one past the inline three: the next
  //    oldest is the oldest, and the rest stay in order.
  // 4. Drop all; a new entry from the free list has one holder.
  locktab_t::part_t t;
  int o[10] = {}, x = 0;
  const lockid_t id{4, 4};
  for (int i = 0; i < 10; i++) {
    t.add(t.find(id), id, &o[i], SM_LOCKMODE_SH, &o[i]);
  }
  locktab_t::entry_t *e = t.find(id);
  REQUIRE(nullptr != e);
  CHECK(locktab_t::oldest(e).owner == &o[0]);
  CHECK(conflicts(e, &x, SM_LOCKMODE_EX).size() == 10);
  CHECK(conflicts(e, &x, SM_LOCKMODE_SH).empty());

  t.drop(id, &o[0], &o[0]);
  t.drop(id, &o[5], &o[5]);
  e = t.find(id);
  REQUIRE(nullptr != e);
  CHECK(locktab_t::oldest(e).owner == &o[1]);
  std::vector<void *> order;
  t.each([&](const lockid_t &, const locktab_t::holder_t &h) {
    order.push_back(h.owner);
  });
  CHECK(order == std::vector<void *>{&o[1], &o[2], &o[3], &o[4], &o[6], &o[7],
                                     &o[8], &o[9]});
  CHECK(nullptr == locktab_t::held(e, &o[5]));
  CHECK(nullptr != locktab_t::held(e, &o[9]));

  for (int i : {1, 2, 3, 4, 6, 7, 8, 9}) {
    t.drop(id, &o[i], &o[i]);
  }
  CHECK(0 == t.size());
  t.add(nullptr, id, &x, SM_LOCKMODE_EX, &x);
  CHECK(1 == holds(t));
  CHECK(locktab_t::oldest(t.find(id)).owner == &x);
}

TEST_CASE("locktab: probe answers as finding the hold, then the conflicts") {
  // Verify the one walk agrees with held() and each_conflict() on
  // every entry of up to five holders of any modes, the asker among
  // them or not, asking any mode.
  // 1. For each count of holders, 1 to 5, and each choice of their
  //    modes, build the entry, the asker the second holder or none.
  // 2. For each mode asked: probe's hold, mode and verdict are those
  //    held() and lock_cover() give, and each_conflict() finds one
  //    holder or more just when probe says so.
  int owner[5] = {}, stranger = 0;
  int cases = 0;
  for (int n = 1; n <= 5; n++) {
    int combos = 1;
    for (int i = 0; i < n; i++) {
      combos *= 5;
    }
    for (int c = 0; c < combos; c++) {
      for (int asker = 0; asker < 2; asker++) {
        locktab_t::part_t t;
        const lockid_t id{8, 8};
        int code = c;
        for (int i = 0; i < n; i++) {
          t.add(t.find(id), id, &owner[i], g_modes[code % 5], &owner[i]);
          code /= 5;
        }
        locktab_t::entry_t *e = t.find(id);
        const void *who = 1 == asker && n > 1 ? &owner[1] : &stranger;
        for (sm_lockmode_t m : g_modes) {
          locktab_t::holder_t *mine;
          sm_lockmode_t want;
          bool conflict = locktab_t::probe(e, who, m, &mine, &want);
          locktab_t::holder_t *h = locktab_t::held(e, who);
          CHECK(mine == h);
          CHECK(want == (nullptr != h ? lock_cover(h->mode, m) : m));
          CHECK(conflict == !conflicts(e, who, want).empty());
          cases++;
        }
      }
    }
  }
  CHECK(cases > 30000);
}
