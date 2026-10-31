/*
 * e07 -- table locks under load: the fast path never grants two
 * incompatible modes at once.
 *
 * Threads lock a few oids in all of Gray's modes, as readers,
 * writers and DDL lock tables: IS and IX by the fast path, in
 * slots of their sessions' own, and S, SIX and X through the lock
 * table, each strong lock sweeping the slots in (SM1_DESIGN.md
 * §5.1). Each thread also converts a lock it holds, rolls back to
 * a savepoint, aborts, prepares, and one opens and closes
 * sessions throughout, while another lists the locks.
 *
 * What each transaction holds is kept beside the engine, in a
 * count per oid and mode, raised after a lock is granted and
 * lowered before it is released. A holder checks, after its
 * count is raised, that no other holder's mode conflicts with its
 * own: of two incompatible holders granted at once, the later to
 * raise its count sees the other's. The run must see none, and
 * end with no entry in the lock table, no slot taken and no
 * strong lock counted.
 */

#include "mvcc.h"

#include "session.h"

#include <cstdio>

#include <atomic>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr int oids = 4;
constexpr int modes = 5;

/* The mode of index i. */
sm1::lockmode_t mode_of(int i) {
  static const sm1::lockmode_t m[modes] = {
      sm1::SM_LOCKMODE_IS, sm1::SM_LOCKMODE_IX, sm1::SM_LOCKMODE_SH,
      sm1::SM_LOCKMODE_SIX, sm1::SM_LOCKMODE_EX};
  return m[i];
}

/* Gray's compatibility, by index, as sm1.h's table says. */
bool compatible(int a, int b) {
  static const bool ok[modes][modes] = {
      {true, true, true, true, false},     {true, true, false, false, false},
      {true, false, true, false, false},   {true, false, false, false, false},
      {false, false, false, false, false},
  };
  return ok[a][b];
}

/* The weakest mode covering both, by index, as a conversion takes. */
int cover(int a, int b) {
  static const int sup[modes][modes] = {
      {0, 1, 2, 3, 4}, {1, 1, 3, 3, 4}, {2, 3, 2, 3, 4},
      {3, 3, 3, 3, 4}, {4, 4, 4, 4, 4},
  };
  return sup[a][b];
}

/* The holders beside the engine: a count per oid and mode. */
struct shadow_t {
  std::atomic<int> held[oids][modes] = {};
  std::atomic<int> violations{0};

  /* A transaction now holds oid in mode i: count it, then check. */
  void grant(int oid, int i) {
    held[oid][i].fetch_add(1);
    for (int m = 0; m < modes; m++) {
      // Our own hold counts once in our mode; any other conflicts.
      int others = held[oid][m].load() - (m == i ? 1 : 0);
      if (others > 0 && !compatible(m, i)) {
        violations++;
      }
    }
  }

  /* A transaction is about to give up oid's lock in mode i. */
  void release(int oid, int i) { held[oid][i].fetch_sub(1); }
};

/*
 * One transaction's locks as the shadow has them: the mode index
 * held on each oid, or -1, and the oids taken after the savepoint.
 */
struct txn_t {
  int mode[oids] = {-1, -1, -1, -1};

  /* Give back every lock, before the engine does. */
  void release_all(shadow_t &sh) {
    for (int o = 0; o < oids; o++) {
      if (mode[o] >= 0) {
        sh.release(o, mode[o]);
        mode[o] = -1;
      }
    }
  }
};

/*
 * Run n transactions on sess. Each locks some oids in ascending
 * order, waiting for each, so none deadlocks; converts a held one
 * only by trylock, which never waits; and ends by commit, abort, a
 * rollback to a savepoint first, or a prepare and its commit. Any
 * failure the run does not expect counts in *other.
 */
void run(sm1::session_t *sess, shadow_t &sh, int seed, int n, int tag,
         std::atomic<int> *other) {
  std::mt19937 rng(seed);
  // A failure the run does not expect: counted, and said, as the
  // count alone would not say which call or why.
  auto fail = [&](const char *what) {
    (*other)++;
    static std::mutex mu;
    std::lock_guard<std::mutex> g(mu);
    fprintf(stderr, "e07: %s failed: %s\n", what, sm1::errmsg());
  };
  auto pick = [&](int below) {
    return static_cast<int>(rng() % static_cast<unsigned>(below));
  };
  // Mostly the weak modes, as tables are read and written.
  auto pick_mode = [&] {
    int r = pick(100);
    return r < 45 ? 0 : r < 80 ? 1 : r < 88 ? 2 : r < 92 ? 3 : 4;
  };
  for (int t = 0; t < n; t++) {
    if (sm1::begin(sess, sm1::xctlevel_t::read_committed) != 0) {
      fail("begin");
      return;
    }
    txn_t x;
    int mark = -1;
    int after_mark = -1;
    bool failed = false;
    for (int o = 0; o < oids && !failed; o++) {
      if (pick(2) == 0) {
        continue;
      }
      // Sometimes the last oid taken is past a savepoint.
      if (mark < 0 && pick(4) == 0) {
        mark = sm1::savepoint(sess);
        if (mark < 0) {
          fail("savepoint");
          failed = true;
          break;
        }
        after_mark = o;
      }
      int i = pick_mode();
      if (sm1::lock_oid(sess, o, mode_of(i)) != 0) {
        fail("lock");
        failed = true;
        break;
      }
      x.mode[o] = i;
      sh.grant(o, i);
      // Sometimes convert it, never waiting: a trylock refused takes
      // nothing, and the transaction goes on.
      if (pick(4) == 0) {
        int j = pick_mode();
        int rc = sm1::trylock_oid(sess, o, mode_of(j));
        if (rc == 0) {
          int c = cover(i, j);
          if (c != i) {
            sh.release(o, i);
            x.mode[o] = c;
            sh.grant(o, c);
          }
        } else if (rc != 1) {
          fail("trylock");
          failed = true;
        }
      }
    }
    if (failed) {
      x.release_all(sh);
      sm1::abort(sess);
      continue;
    }
    switch (pick(8)) {
    case 0:
      // Abort: every lock goes.
      x.release_all(sh);
      sm1::abort(sess);
      break;
    case 1: {
      // Prepare, then commit by gid: the locks move to the prepared
      // transaction, and go at its commit.
      std::string gid = "g" + std::to_string(tag) + "." + std::to_string(t);
      if (sm1::prepare(sess, gid) != 0) {
        fail("prepare");
        x.release_all(sh);
        sm1::abort(sess);
        break;
      }
      x.release_all(sh);
      if (sm1::commit_prepared(sess, gid) != 0) {
        fail("commit_prepared");
      }
      break;
    }
    default:
      // A rollback to the savepoint gives back the oids past it,
      // none converted before it, then the commit the rest.
      if (mark > 0) {
        for (int o = after_mark; o < oids; o++) {
          if (x.mode[o] >= 0) {
            sh.release(o, x.mode[o]);
            x.mode[o] = -1;
          }
        }
        if (sm1::rollback(sess, mark) != 0) {
          fail("rollback");
        }
      }
      x.release_all(sh);
      if (sm1::commit(sess) != 0) {
        fail("commit");
      }
    }
  }
}

} // namespace

TEST_CASE("e07: table locks never grant two incompatible modes at once") {
  scenario_t s;
  shadow_t sh;
  std::atomic<int> other{0};
  std::atomic<bool> done{false};
  std::vector<std::thread> threads;
  constexpr int workers = 8;
  for (int i = 0; i < workers; i++) {
    sm1::session_t *sess = s.session(1 + i);
    threads.emplace_back(
        [&, sess, i] { run(sess, sh, 7 + i, 1500, i, &other); });
  }
  // Sessions come and go while strong locks sweep.
  std::thread churn([&] {
    for (int k = 0; !done; k++) {
      sm1::session_t *c = nullptr;
      if (sm1::opendb(s.env(), "db", c) != 0) {
        other++;
        return;
      }
      run(c, sh, 1000 + k, 3, 100 + k, &other);
      sm1::closedb(c);
    }
  });
  // And the locks are listed, every slot and the table.
  std::thread status([&] {
    std::vector<sm1::lock_status_t> out;
    while (!done) {
      if (sm1::lock_status(s.env(), out) != 0) {
        other++;
      }
    }
  });
  for (std::thread &th : threads) {
    th.join();
  }
  done = true;
  churn.join();
  status.join();

  CHECK(sh.violations == 0);
  CHECK(other == 0);
  sm1::session_t *s1 = s.session(1);
  {
    wisr::lock_all_t all(s1->db);
    CHECK(s1->db->lock_table.empty());
  }
  for (const auto &c : s1->db->fp_strong) {
    CHECK(0 == c.load());
  }
  for (int i = 0; i < workers; i++) {
    CHECK(s.session(1 + i)->fp_used == 0);
  }
}
