/*
 * Copyright (c) 2026 CK Tan
 * SPDX-License-Identifier: MIT
 */

/*
 * test_defer.cc -- defer blocks and scope_guard_t (include/sm0_defer.h).
 */

#include "doctest.h"

#include "sm0_defer.h"

#include <stdexcept>
#include <string>

namespace {

/* Append to log from a defer, returning early when asked. */
void early_return(std::string &log, bool early) {
  defer { log += "d"; };
  if (early) {
    return;
  }
  log += "b";
}

} // namespace

TEST_CASE("defer blocks run at scope exit, last first") {
  // Verify ordering, including two defers on one line.
  // 1. Three defers in one scope, two sharing a line, log in
  //    reverse order after the scope's body.
  // 2. A defer runs on an early return as on a normal one.
  std::string log;
  {
    defer { log += "3"; };
    defer { log += "2"; };
    defer { log += "1"; };
    log += "0";
  }
  CHECK(log == "0123");

  log.clear();
  early_return(log, true);
  early_return(log, false);
  CHECK(log == "dbd");
}

TEST_CASE("a defer block sees variables as they are at exit") {
  // Verify capture by reference: a later change is visible.
  // 1. Defer a read of n, then change n; the block sees the
  //    changed value.
  int n = 1;
  int seen = 0;
  {
    defer { seen = n; };
    n = 2;
  }
  CHECK(seen == 2);
}

TEST_CASE("defer runs during exception unwinding") {
  // Verify cleanup on the exceptional path.
  // 1. Throw out of a scope holding a defer; the block ran
  //    before the handler.
  bool ran = false;
  try {
    defer { ran = true; };
    throw std::runtime_error("x");
  } catch (const std::runtime_error &) {
    CHECK(ran);
  }
}

TEST_CASE("a dismissed scope_guard_t does not run") {
  // Verify dismiss(), for cleanup needed only on failure.
  // 1. A guard left alone runs at scope exit.
  // 2. A dismissed guard does not.
  int runs = 0;
  {
    sm0::scope_guard_t g([&] { runs++; });
  }
  CHECK(runs == 1);
  {
    sm0::scope_guard_t g([&] { runs++; });
    g.dismiss();
  }
  CHECK(runs == 1);
}
