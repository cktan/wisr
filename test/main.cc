/*
 * main.cc -- the doctest runner. Every other file in test/
 * contributes test cases; this one supplies main().
 *
 * `test_wisr --<name>-child <homedir>` runs a child workload
 * instead of the tests: crash (test_crash.cc), prepare and
 * orphan (test_2pc.cc), and seq (test_seq.cc). Those tests
 * re-execute this binary rather than forking, so the child
 * starts without the parent's RocksDB threads.
 */

#define DOCTEST_CONFIG_IMPLEMENT
#include "doctest.h"

#include <string.h>

/* The crash test's child workload; never returns. */
int crash_child_main(const char *homedir);

/* test_2pc.cc's child workloads: they report, then wait to be killed. */
int prepare_child_main(const char *homedir);
int orphan_child_main(const char *homedir);

/* test_seq.cc's child workload: reports each value, until killed. */
int seq_child_main(const char *homedir);

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
  if (argc == 3 && strcmp(argv[1], "--seq-child") == 0) {
    return seq_child_main(argv[2]);
  }
  return doctest::Context(argc, argv).run();
}
