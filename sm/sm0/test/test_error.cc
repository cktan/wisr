/*
 * test_error.cc -- the calling thread's error state (sm0.h) and
 * the helpers that set it (sm0_error.h).
 */

#include "doctest.h"

#include "sm0.h"
#include "sm0_error.h"

#include <errno.h>
#include <string.h>

#include <string>
#include <thread>

TEST_CASE("an error is recorded, named, and cleared per thread") {
  // Verify the state's life cycle and that threads do not share it.
  // 1. Clear the state and check it reads SM_OK, "no error".
  // 2. Set a retryable error and check its code, message, name.
  // 3. Check another thread still sees its own clean state.
  // 4. Clear it again and check it is back to SM_OK.
  sm0::reseterr();
  CHECK(sm0::errnum() == sm0::SM_OK);
  CHECK(std::string(sm0::errmsg()) == "no error");
  CHECK(std::string(sm0::errname()) == "OK");

  CHECK(sm0::seterr(sm0::SM_ERR_CONFLICT, "busy") == -1);
  CHECK(sm0::errnum() == sm0::SM_ERR_CONFLICT);
  CHECK(std::string(sm0::errmsg()) == "busy");
  CHECK(std::string(sm0::errname()) == "ERR_CONFLICT");
  CHECK(sm0::retryable() == 1);

  sm0::errnum_t other = sm0::SM_ERR_IO;
  std::thread([&] { other = sm0::errnum(); }).join();
  CHECK(other == sm0::SM_OK);

  sm0::reseterr();
  CHECK(sm0::errnum() == sm0::SM_OK);
  CHECK(sm0::retryable() == 0);
}

TEST_CASE("a duplicate key's record lasts until the next error") {
  // Verify setdup() annotates the error just set, and only it.
  // 1. Set a duplicate key error and check it knows no record.
  // 2. Record an index and two rows, and read them back.
  // 3. Set another error, by each setter, and check it is gone.
  // 4. Record one again, clear the state, and check it is gone.
  sm0::seterr(sm0::SM_ERR_DUPKEY, "key already exists");
  CHECK(sm0::errdup().index == 0);
  CHECK(sm0::errdup().nrows == 0);

  sm0::setdup({7, {3, 9}, 2});
  CHECK(sm0::errdup().index == 7);
  CHECK(sm0::errdup().rownos[0] == 3);
  CHECK(sm0::errdup().rownos[1] == 9);
  CHECK(sm0::errdup().nrows == 2);

  sm0::seterr(sm0::SM_ERR_IO, "disk");
  CHECK(sm0::errdup().nrows == 0);
  sm0::setdup({7, {3, 9}, 2});
  sm0::set_error(sm0::SM_ERR_IO, "{}", "disk");
  CHECK(sm0::errdup().index == 0);

  sm0::setdup({7, {3, 0}, 1});
  sm0::reseterr();
  CHECK(sm0::errdup().index == 0);
  CHECK(sm0::errdup().nrows == 0);
}

TEST_CASE("a message too long for the buffer is cut short") {
  // Verify both setters truncate rather than overrun.
  // 1. Set a message far longer than the buffer with seterr.
  // 2. Check it was cut and is still NUL-terminated.
  // 3. Do the same through the formatting set_error.
  std::string big(100000, 'x');
  sm0::seterr(sm0::SM_ERR_INVALID, big);
  size_t n = strlen(sm0::errmsg());
  CHECK(n > 0);
  CHECK(n < big.size());

  sm0::set_error(sm0::SM_ERR_INVALID, "{}", big);
  CHECK(strlen(sm0::errmsg()) == n);
}

TEST_CASE("the helpers map their inputs onto codes") {
  // Verify the errno mapping and the name checks.
  // 1. Map ENOENT and ENOSPC and check codes and message.
  // 2. Reject NULL, empty and over-long names; accept a good one.
  // 3. Reject a name holding a NUL; copy a good one.
  CHECK(sm0::set_errno_error(ENOENT, "open") == -1);
  CHECK(sm0::errnum() == sm0::SM_ERR_NOTFOUND);
  CHECK(std::string(sm0::errmsg()).starts_with("open: "));
  sm0::set_errno_error(ENOSPC, "write");
  CHECK(sm0::errnum() == sm0::SM_ERR_DISKFULL);

  CHECK(sm0::check_name(nullptr, 8, "store") == -1);
  CHECK(sm0::errnum() == sm0::SM_ERR_INVALID);
  CHECK(sm0::check_name("", 8, "store") == -1);
  CHECK(sm0::check_name("toolongname", 8, "store") == -1);
  CHECK(sm0::check_name("ok", 8, "store") == 0);

  std::string out;
  CHECK(sm0::copy_name(std::string_view("a\0b", 3), "db", out) == -1);
  CHECK(sm0::copy_name("good", "db", out) == 0);
  CHECK(out == "good");
}
