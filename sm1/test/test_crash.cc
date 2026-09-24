/*
 * test_crash.cc -- kill a committing process with SIGKILL and
 * check that recovery leaves exactly a prefix of its commits,
 * including every commit it acknowledged (SM1_DESIGN.md §9, §12.5).
 */

#include "doctest.h"
#include "util.h"

#include "sm1.h"

#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>

#include <random>
#include <string>

namespace {

/* The key of the i-th commit; fixed width, so memcmp order is i order. */
std::string crash_key(uint32_t i) {
  char buf[16];
  snprintf(buf, sizeof(buf), "k%08u", i);
  return buf;
}

/*
 * The database's one store, as s's transaction sees it: the
 * lowest storeid listed, which is the first store created.
 * Returns 0 if there is none.
 */
uint64_t first_store(sm1::session_t *s) {
  uint64_t *list = nullptr;
  size_t n = 0;
  if (sm1_lsstore(s, &list, &n) != 0 || n == 0) {
    return 0;
  }
  uint64_t store = list[0];
  free(list);
  return store;
}

/* The number of rows in store, as s reads them. */
uint32_t count_rows(sm1::session_t *s, uint64_t store) {
  sm1::scan_t *c = sm1_scan(s, store, nullptr, 0, 0, nullptr, 0, 0);
  if (!c) {
    return UINT32_MAX;
  }
  uint32_t n = 0;
  const char *k, *d;
  size_t kl, dl;
  while (sm1_scannxt(c, &k, &kl, &d, &dl) == 0) {
    n++;
  }
  sm1_scanend(c);
  return n;
}

/* Read exactly n bytes from fd; false at EOF or on error. */
bool read_full(int fd, void *buf, size_t n) {
  char *p = static_cast<char *>(buf);
  while (n > 0) {
    ssize_t r = read(fd, p, n);
    if (r <= 0) {
      return false;
    }
    p += r;
    n -= r;
  }
  return true;
}

} // namespace

/*
 * The child: append crash_key(n), crash_key(n + 1), ... to "t",
 * one per transaction, and after each commit returns write its
 * index to stdout. Runs until killed.
 */
int crash_child_main(const char *homedir) {
  sm1::env_t *env = sm1_attach(homedir);
  if (!env) {
    return 2;
  }
  sm1::session_t *s = sm1_opendb(env, "db", nullptr);
  if (!s || sm1_begin(s, sm1::xctlevel_t::repeatable_read) != 0) {
    return 3;
  }
  // Find the store, then leave the transaction that read it.
  uint64_t store = first_store(s);
  uint32_t next = store ? count_rows(s, store) : UINT32_MAX;
  if (next == UINT32_MAX || sm1_commit(s) != 0) {
    return 4;
  }
  for (uint32_t i = next;; i++) {
    std::string k = crash_key(i);
    if (sm1_begin(s, sm1::xctlevel_t::repeatable_read) != 0 ||
        sm1_insert(s, store, k.data(), k.size(), "v", 1) != 0 ||
        sm1_commit(s) != 0) {
      return 5;
    }
    if (write(1, &i, sizeof(i)) != sizeof(i)) {
      return 6;
    }
  }
}

TEST_CASE("a killed process leaves a prefix of its commits, acknowledged ones "
          "included") {
  // Verify crash recovery against SIGKILL at arbitrary points.
  // 1. Create database "db" with store 1.
  // 2. Each round, start a child that commits one key per
  //    transaction and reports each acknowledged commit.
  // 3. After a random number of acknowledgements, SIGKILL it
  //    and collect every acknowledgement it sent.
  // 4. Reattach: the keys present must be exactly k0..k(n-1)
  //    for some n covering every acknowledged commit.
  // 5. Repeat, each child continuing where recovery left off.
  tmpdir_t tmp;
  REQUIRE(sm1_init(tmp.path()) == 0);
  {
    sm1::env_t *env = sm1_attach(tmp.path());
    REQUIRE(env);
    REQUIRE(sm1_mkdb(env, "db", nullptr) == 0);
    sm1::session_t *s = sm1_opendb(env, "db", nullptr);
    REQUIRE(s);
    REQUIRE(sm1_begin(s, sm1::xctlevel_t::repeatable_read) == 0);
    mkstore(s);
    REQUIRE(sm1_commit(s) == 0);
    REQUIRE(sm1_detach(env, 1) == 0);
  }

  std::mt19937 rng(20260918);
  uint32_t acked = 0; // commits acknowledged across all rounds
  for (int round = 0; round < 5; round++) {
    CAPTURE(round);
    int fds[2];
    REQUIRE(pipe(fds) == 0);
    pid_t pid = fork();
    REQUIRE(pid >= 0);
    if (pid == 0) {
      // In the child: stdout becomes the pipe, then exec.
      dup2(fds[1], 1);
      close(fds[0]);
      close(fds[1]);
      execl("/proc/self/exe", "test_wisr", "--crash-child", tmp.path(),
            (char *)nullptr);
      _exit(127);
    }
    close(fds[1]);

    uint32_t target = acked + 20 + rng() % 300;
    uint32_t i;
    while (acked < target && read_full(fds[0], &i, sizeof(i))) {
      CHECK(i == acked);
      acked = i + 1;
    }
    REQUIRE_MESSAGE(acked >= target, "child stopped early");
    // Kill the child mid-workload, then collect what it acked.
    kill(pid, SIGKILL);
    int status;
    REQUIRE(waitpid(pid, &status, 0) == pid);
    CHECK(WIFSIGNALED(status));
    while (read_full(fds[0], &i, sizeof(i))) {
      acked = i + 1;
    }
    close(fds[0]);

    sm1::env_t *env = sm1_attach(tmp.path());
    REQUIRE_MESSAGE(env, sm1::errmsg());
    sm1::session_t *s = sm1_opendb(env, "db", nullptr);
    REQUIRE_MESSAGE(s, sm1::errmsg());
    REQUIRE(sm1_begin(s, sm1::xctlevel_t::repeatable_read) == 0);
    uint64_t store = first_store(s);
    REQUIRE(store != 0);
    uint32_t n = count_rows(s, store);
    CHECK(n >= acked);
    CHECK(n <= acked + 1); // at most the one in flight at the kill
    for (uint32_t j = 0; j < n; j++) {
      std::string k = crash_key(j);
      CHECK(get(s, store, k) == "v");
    }
    REQUIRE(sm1_commit(s) == 0);
    REQUIRE(sm1_detach(env, 1) == 0);
    acked = n;
  }
}
