/*
 * test_fatal.cc -- sm0::fatal reports on stderr and to the log
 * hook, then aborts the process.
 */

#include "doctest.h"
#include "sm0_log.h"

#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstring>
#include <string>

namespace {

/* The log hook's file descriptor in the child. */
int g_hook_fd = -1;

/* A log hook that writes its level and message to g_hook_fd. */
void hook(void *, sm0::loglevel_t level, const char *msg) noexcept {
  std::string line = std::to_string(static_cast<int>(level)) + " " + msg;
  ssize_t n = write(g_hook_fd, line.data(), line.size());
  (void)n;
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

} // namespace

TEST_CASE("sm0::fatal reports on stderr and the log hook, then aborts") {
  // Verify a fatal error is seen by an operator either way, and ends
  // the process.
  // 1. Fork a child whose stderr and log hook each write to a pipe.
  // 2. The child calls sm0::fatal with a formatted message.
  // 3. It dies of SIGABRT.
  // 4. Its stderr and its log hook each hold the message, after
  //    "*** FATAL ***"; the hook got it at the error level.
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
    // Install the hook, which the fatal error reports to.
    sm0::set_log(hook, nullptr);
    sm0::fatal("the {} broke at {}", "disk", 42);
  }
  close(err_fds[1]);
  close(hook_fds[1]);
  int status;
  REQUIRE(waitpid(pid, &status, 0) == pid);
  std::string err = read_all(err_fds[0]);
  std::string logged = read_all(hook_fds[0]);
  close(err_fds[0]);
  close(hook_fds[0]);

  CAPTURE(err);
  CAPTURE(logged);
  REQUIRE(WIFSIGNALED(status));
  CHECK(WTERMSIG(status) == SIGABRT);
  CHECK(err == "*** FATAL *** the disk broke at 42\n");
  CHECK(logged == "0 *** FATAL *** the disk broke at 42");
}
