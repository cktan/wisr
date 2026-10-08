/*
 * e01 -- SSI state does not accumulate.
 *
 * Five SERIALIZABLE transactions read, scan, write, and
 * commit, one after another. With none of them still active,
 * a GC run must leave no node, no SIREAD entry, and no range
 * behind (SM1_DESIGN.md §7.1): tracking that never released its
 * state would grow without bound on a busy database.
 *
 * This scenario reaches into the database object, so it is the
 * one file in mvcc/ that includes the engine's own headers.
 */

#include "mvcc.h"

#include "db.h"
#include "session.h"

#include <mutex>
#include <string>

TEST_CASE("e01: SSI state is freed once no transaction overlaps it") {
  scenario_t s({R1, R2});
  sm1::session_t *t = s.session(1);
  for (int round = 0; round < 5; round++) {
    s.begin_at(1, sm1::xctlevel_t::serializable);
    CHECK(s.r(1, R1) == "0");
    CHECK(s.s(1, R1, R2).size() == 2);
    REQUIRE(s.w(1, R2, std::to_string(round)) == 0);
    CHECK(s.c(1));
  }
  wisr::gc_run_once(t->db);

  std::lock_guard<std::mutex> guard(t->db->ssi_mu);
  CHECK(t->db->ssi_nodes.empty());
  CHECK(t->db->ssi_points.empty());
  CHECK(t->db->ssi_by_xid.empty());
  for (auto &kv : t->db->ssi_ranges) {
    CHECK(kv.second.empty());
  }
}
