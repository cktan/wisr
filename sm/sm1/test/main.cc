/*
 * Copyright (c) 2026 CK Tan
 * SPDX-License-Identifier: MIT
 */

/*
 * main.cc -- the doctest runner. Every other file in test/
 * contributes test cases; this one supplies main().
 *
 * `test_sm1 --<name>-child <homedir>` runs a child workload
 * instead of the tests: crash (test_crash.cc), prepare and
 * orphan, prepare-fail and abort-fail (test_2pc.cc), seq
 * (test_seq.cc), oom (test_env.cc),
 * commit-fail (test_xct.cc) and sync-fail (test_groupsync.cc).
 * Those tests
 * re-execute this binary rather than forking, so the child
 * starts without the parent's RocksDB threads.
 */

#define DOCTEST_CONFIG_IMPLEMENT
#include "doctest.h"
#include "sm0_doctest_active.h"
#include "util.h"

#include <string.h>

/* The crash test's child workload; never returns. */
int crash_child_main(const char *homedir);

/* test_2pc.cc's child workloads: they report, then wait to be killed. */
int prepare_child_main(const char *homedir);
int orphan_child_main(const char *homedir);

/* test_2pc.cc's child workloads: fail a 2PC record write, aborting. */
int prepare_fail_child_main(const char *homedir);
int abort_fail_child_main(const char *homedir);

/* test_seq.cc's child workload: reports each value, until killed. */
int seq_child_main(const char *homedir);

/* In test_seq.cc: draw a sequence to its end, then die. */
int seq_end_child_main(const char *homedir);

/* test_env.cc's child workload: runs out of memory. */
int oom_child_main(const char *homedir);

/* test_xct.cc's child workload: fails a commit's status write. */
int commit_fail_child_main(const char *homedir);

/* test_groupsync.cc's child workload: fails a commit's WAL sync. */
int sync_fail_child_main(const char *homedir);

/* Run a child workload if asked to, else the tests. */
int main(int argc, char **argv) {
  if (argc == 3 && strcmp(argv[1], "--crash-child") == 0) {
    return crash_child_main(argv[2]);
  }
  if (argc == 3 && strcmp(argv[1], "--prepare-child") == 0) {
    return prepare_child_main(argv[2]);
  }
  if (argc == 3 && strcmp(argv[1], "--orphan-child") == 0) {
    return orphan_child_main(argv[2]);
  }
  if (argc == 3 && strcmp(argv[1], "--prepare-fail-child") == 0) {
    return prepare_fail_child_main(argv[2]);
  }
  if (argc == 3 && strcmp(argv[1], "--abort-fail-child") == 0) {
    return abort_fail_child_main(argv[2]);
  }
  if (argc == 3 && strcmp(argv[1], "--seq-child") == 0) {
    return seq_child_main(argv[2]);
  }
  if (argc == 3 && strcmp(argv[1], "--seq-end-child") == 0) {
    return seq_end_child_main(argv[2]);
  }
  if (argc == 3 && strcmp(argv[1], "--oom-child") == 0) {
    return oom_child_main(argv[2]);
  }
  if (argc == 3 && strcmp(argv[1], "--commit-fail-child") == 0) {
    return commit_fail_child_main(argv[2]);
  }
  if (argc == 3 && strcmp(argv[1], "--sync-fail-child") == 0) {
    return sync_fail_child_main(argv[2]);
  }
  return doctest::Context(argc, argv).run();
}
