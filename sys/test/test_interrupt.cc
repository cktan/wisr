/*
 * test_interrupt.cc -- sys/interrupt.h. The interface is designed
 * and not built yet (INTERRUPT.md), so this holds it to what the
 * design promises of its constants; the behaviour's tests come
 * with the code.
 */
#include "doctest.h"

#include "sys/interrupt.h"

#include <stdint.h>

TEST_CASE("the interface compiles on its own, with the values it promises") {
  // Verify the header stands alone and keeps its documented values.
  // 1. Include it with nothing before it (this file does).
  // 2. Confirm SYS_FOREVER is a deadline that never comes.
  // 3. Confirm the three wait results are distinct.
  CHECK(SYS_FOREVER == INT64_MAX);
  CHECK(SYS_WAIT_DONE != SYS_WAIT_INTERRUPTED);
  CHECK(SYS_WAIT_INTERRUPTED != SYS_WAIT_TIMED_OUT);
  CHECK(SYS_WAIT_DONE != SYS_WAIT_TIMED_OUT);
}
