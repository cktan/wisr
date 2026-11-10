/*
 * Copyright (c) 2026 CK Tan
 * SPDX-License-Identifier: MIT
 */

/*
 * test_fatal.cc -- sm0::fatal reports on stderr and to the log
 * hook, then aborts the process, even when the hook re-enters it
 * or does not return.
 */

#include "doctest.h"
#include "sm0_log.h"

#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <cstring>
#include <string>
#include <thread>

namespace {

/* The log hook's file descriptor in the child. */
int g_hook_fd = -1;

/* Set by stall_hook once it has written, and is about to stall. */
std::atomic<bool> g_stalled{false};

/* A log hook that writes its level and message to g_hook_fd. */
void hook(void *, sm0::loglevel_t level, const char *msg) noexcept {
  std::string line = std::to_string(static_cast<int>(level)) + " " + msg;
  ssize_t n = write(g_hook_fd, line.data(), line.size());
  (void)n;
}

/* A log hook that writes, then fails fatally itself. */
void fatal_hook(void *arg, sm0::loglevel_t level, const char *msg) noexcept {
  hook(arg, level, msg);
  sm0::fatal("the hook broke");
}

/* A log hook that writes, then never returns. */
void stall_hook(void *arg, sm0::loglevel_t level, const char *msg) noexcept {
  hook(arg, level, msg);
  g_stalled.store(true);
  for (;;) {
    pause();
  }
}

/* Read fd until its end. */
std::string read_all(int fd) {
  std::string out;
  char buf[512];
  ssize_t n;
  while ((n = read(fd, buf, sizeof(buf))) > 0) {
    out.append(buf, n);
  }
  return out;
}

/* What a child left: its wait status, its stderr, its hook's output. */
struct child_result_t {
  int status;
  std::string err;
  std::string logged;
};

/*
 * Run body in a forked child whose stderr and log hook each write
 * to a pipe, and collect what it left.
 * 1. Make the two pipes and fork.
 * 2. In the child, point stderr and g_hook_fd at the pipes, let
 *    SIGABRT kill it, arm a 10-second alarm against a hang, and
 *    run body, which must not return.
 * 3. In the parent, wait for the child and read both pipes.
 */
child_result_t run_fatal_child(void (*body)()) {
  int err_fds[2], hook_fds[2];
  REQUIRE(pipe(err_fds) == 0);
  REQUIRE(pipe(hook_fds) == 0);
  pid_t pid = fork();
  REQUIRE(pid >= 0);
  if (pid == 0) {
    // In the child: stderr and the hook become the pipes.
    dup2(err_fds[1], 2);
    g_hook_fd = hook_fds[1];
    close(err_fds[0]);
    close(hook_fds[0]);
    // Die of the abort, not of doctest's handler, which reports a
    // crash as the parent's.
    signal(SIGABRT, SIG_DFL);
    // Turn a hang into a death by SIGALRM, which the test reports.
    alarm(10);
    body();
    _exit(0);
  }
  close(err_fds[1]);
  close(hook_fds[1]);
  child_result_t r;
  REQUIRE(waitpid(pid, &r.status, 0) == pid);
  r.err = read_all(err_fds[0]);
  r.logged = read_all(hook_fds[0]);
  close(err_fds[0]);
  close(hook_fds[0]);
  return r;
}

} // namespace

TEST_CASE("sm0::fatal reports on stderr and the log hook, then aborts") {
  // Verify a fatal error is seen by an operator either way, and ends
  // the process.
  // 1. Fork a child whose stderr and log hook each write to a pipe.
  // 2. The child calls sm0::fatal with a formatted message.
  // 3. It dies of SIGABRT.
  // 4. Its stderr and its log hook each hold the message, after
  //    "*** FATAL ***"; the hook got it at the error level.
  child_result_t r = run_fatal_child([] {
    // Install the hook, which the fatal error reports to.
    sm0::set_log(hook, nullptr);
    sm0::fatal("the {} broke at {}", "disk", 42);
  });

  CAPTURE(r.err);
  CAPTURE(r.logged);
  REQUIRE(WIFSIGNALED(r.status));
  CHECK(WTERMSIG(r.status) == SIGABRT);
  CHECK(r.err == "*** FATAL *** the disk broke at 42\n");
  CHECK(r.logged == "0 *** FATAL *** the disk broke at 42");
}

TEST_CASE("sm0::fatal called from the log hook aborts") {
  // Verify a hook that fails fatally does not deadlock on the hook's
  // own mutex, which its thread already holds.
  // 1. Fork a child with a hook that writes, then calls sm0::fatal.
  // 2. The child logs one message.
  // 3. It dies of SIGABRT, not of the alarm.
  // 4. Its stderr holds the hook's fatal error; the hook ran once.
  child_result_t r = run_fatal_child([] {
    // Install the hook that fails.
    sm0::set_log(fatal_hook, nullptr);
    sm0::log_msg(sm0::loglevel_t::warn, "the {} is slow", "disk");
  });

  CAPTURE(r.err);
  CAPTURE(r.logged);
  REQUIRE(WIFSIGNALED(r.status));
  CHECK(WTERMSIG(r.status) == SIGABRT);
  CHECK(r.err == "*** FATAL *** the hook broke\n");
  CHECK(r.logged == "1 the disk is slow");
}

TEST_CASE("sm0::fatal aborts while another thread's hook is stuck") {
  // Verify a hook that never returns delays a fatal error by a
  // bounded time only, and stderr still reports it.
  // 1. Fork a child with a hook that writes, then never returns.
  // 2. A second thread logs, and stalls in the hook.
  // 3. The main thread calls sm0::fatal.
  // 4. The child dies of SIGABRT, not of the alarm.
  // 5. Its stderr holds the fatal error; the hook got only the
  //    stalled message.
  child_result_t r = run_fatal_child([] {
    // Install the hook that stalls.
    sm0::set_log(stall_hook, nullptr);
    // Stall a second thread in the hook, holding its mutex.
    std::thread([] {
      sm0::log_msg(sm0::loglevel_t::info, "stalled");
    }).detach();
    while (!g_stalled.load()) {
      std::this_thread::yield();
    }
    sm0::fatal("the {} broke", "disk");
  });

  CAPTURE(r.err);
  CAPTURE(r.logged);
  REQUIRE(WIFSIGNALED(r.status));
  CHECK(WTERMSIG(r.status) == SIGABRT);
  CHECK(r.err == "*** FATAL *** the disk broke\n");
  CHECK(r.logged == "2 stalled");
}
