/*
 * e04 -- the catalog is read under the same rules.
 *
 *   T1: sm_lsrel        T2: sm_lsrel
 *   T1: sm_mkrel        T2: sm_mkrel
 *   T1: c               T2: c
 *
 * sm_lsrel is a scan of the catalog relation, so each
 * transaction's create lands in the other's predicate. The
 * pattern is c03 with relations instead of rows, and it must
 * behave the same way.
 *
 * Expected:
 *   RR    both commit
 *   RR+V  both commit
 *   V+V   at most one commits
 */

#include "mvcc.h"

TEST_CASE("e04: listing relations takes part in conflicts like any scan") {
  for (pairing_t p : MVCC_PAIRINGS) {
    PAIRING(p);
    scenario_t s;
    s.begin(1, p);
    s.begin(2, p);

    uint64_t *list = nullptr;
    size_t n = 0;
    REQUIRE(sm_lsrel(s.session(1), &list, &n) == 0);
    CHECK(n == 1); // the scenario's own relation
    sm_free_rels(list);
    REQUIRE(sm_lsrel(s.session(2), &list, &n) == 0);
    CHECK(n == 1);
    sm_free_rels(list);

    uint64_t r1 = 0, r2 = 0;
    REQUIRE(sm_mkrel(s.session(1), &r1) == 0);
    REQUIRE(sm_mkrel(s.session(2), &r2) == 0);
    CHECK(r1 != r2);

    bool c1 = s.c(1), c2 = s.c(2);
    if (both_serializable(p)) {
      CHECK_FALSE((c1 && c2));
    } else {
      CHECK(c1);
      CHECK(c2);
    }
  }
}
