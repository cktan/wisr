/*
 * main.cc -- the doctest runner. Every other file in test/
 * contributes test cases; this one supplies main().
 *
 * `test_wisr --crash-child <homedir>` runs the crash test's
 * child workload instead of the tests (test_crash.cc). The
 * crash test re-executes this binary rather than forking, so
 * the child starts without the parent's RocksDB threads.
 */

#define DOCTEST_CONFIG_IMPLEMENT
#include "doctest.h"

#include <string.h>

/* The crash test's child workload; never returns. */
int crash_child_main(const char* homedir);

/* Run the child workload if asked to, else the tests. */
int main(int argc, char** argv)
{
    if (argc == 3 && strcmp(argv[1], "--crash-child") == 0) {
        return crash_child_main(argv[2]);
    }
    return doctest::Context(argc, argv).run();
}
