/*
 * test_error.cc -- the per-thread error state, and how a failed
 * call reports through it (sq.h).
 */

#include "doctest.h"

#include "sq.h"

#include "error.h"

#include <string.h>

#include <string>
#include <thread>

TEST_CASE("a thread starts with no error") {
  // Verify the error state a thread has before any call fails.
  // 1. Start a thread that has made no call.
  // 2. Read its code, message, SQLSTATE and position.
  std::thread([] {
    CHECK(sq_errnum() == SQ_OK);
    CHECK(std::string(sq_errmsg()) == "no error");
    CHECK(std::string(sq_sqlstate()) == "00000");
    CHECK(sq_errpos() == 0);
  }).join();
}

TEST_CASE("an error records its code, SQLSTATE, position and message") {
  // Verify set_error returns -1 and every accessor reads back what it set.
  // 1. Record a syntax error at character 7 with a formatted message.
  // 2. Read back the code, message, SQLSTATE and position.
  CHECK(sq::set_error(SQ_ERR_PARSE, sq::sqlstate_syntax_error, 7,
                      "syntax error at or near \"{}\"", "WHERE") == -1);
  CHECK(sq_errnum() == SQ_ERR_PARSE);
  CHECK(std::string(sq_errmsg()) == "syntax error at or near \"WHERE\"");
  CHECK(std::string(sq_sqlstate()) == "42601");
  CHECK(sq_errpos() == 7);
}

TEST_CASE("a message too long for the buffer is cut, not overrun") {
  // Verify an over-long message is truncated and still terminated.
  // 1. Record an error whose message is far longer than the buffer.
  // 2. Confirm the message read back is shorter, and all of one letter.
  std::string big(100000, 'x');
  CHECK(sq::set_error(SQ_ERR_PARSE, sq::sqlstate_syntax_error, 0, "{}", big) ==
        -1);
  size_t n = strlen(sq_errmsg());
  CHECK(n > 0);
  CHECK(n < big.size());
  CHECK(std::string(sq_errmsg()) == std::string(n, 'x'));
}

TEST_CASE("one thread's error is not another's") {
  // Verify the error state is per thread.
  // 1. Record an error on this thread.
  // 2. Confirm a new thread sees no error.
  // 3. Record a different error there, and confirm this thread's stands.
  sq::set_error(SQ_ERR_PARSE, sq::sqlstate_syntax_error, 3, "here");
  std::thread([] {
    CHECK(sq_errnum() == SQ_OK);
    sq::set_error(SQ_ERR_NOMEM, sq::sqlstate_out_of_memory, 0, "there");
    CHECK(sq_errnum() == SQ_ERR_NOMEM);
  }).join();
  CHECK(sq_errnum() == SQ_ERR_PARSE);
  CHECK(std::string(sq_errmsg()) == "here");
  CHECK(sq_errpos() == 3);
}
