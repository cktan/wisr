/*
 * Copyright (c) 2026 CK Tan
 * SPDX-License-Identifier: MIT
 */

/*
 * error.h -- setting the calling thread's error state, and raising.
 *
 * set_error records an error for sq_errnum(), sq_errmsg(),
 * sq_sqlstate() and sq_errpos() and returns -1, so that a call can
 * write `return set_error(...)`. Nothing here clears the state: a
 * successful call leaves it untouched (sq.h).
 *
 * Inside a parse an error is raised: raise() records it and throws
 * parse_error_t, which sq_parse catches. It is how the scanner and
 * the grammar leave from wherever they are, as PostgreSQL's leave by
 * longjmp, and no exception of it is ever seen outside sq.
 */
#ifndef SQ_SRC_ERROR_H
#define SQ_SRC_ERROR_H

#include <format>
#include <string_view>

#include "sq_error.h"

namespace sq {

/* The SQLSTATEs sq raises with more than one caller. */
inline constexpr char sqlstate_syntax_error[] = "42601";
inline constexpr char sqlstate_feature_not_supported[] = "0A000";
inline constexpr char sqlstate_out_of_memory[] = "53200";
inline constexpr char sqlstate_internal_error[] = "XX000";
inline constexpr char sqlstate_character_not_in_repertoire[] = "22021";
inline constexpr char sqlstate_invalid_escape_sequence[] = "22025";
inline constexpr char sqlstate_nonstandard_use_of_escape_character[] = "22P06";
inline constexpr char sqlstate_name_too_long[] = "42622";
inline constexpr char sqlstate_invalid_parameter_value[] = "22023";
inline constexpr char sqlstate_windowing_error[] = "42P20";
inline constexpr char sqlstate_reserved_name[] = "42939";
inline constexpr char sqlstate_duplicate_object[] = "42710";
inline constexpr char sqlstate_warning[] = "01000";

/*
 * Record e with its SQLSTATE, a position in characters from 1 or 0
 * for none, and a message formatted from fmt and args. Returns -1.
 */
int set_error_v(sq_errnum_t e, const char *sqlstate, int pos,
                std::string_view fmt, std::format_args args);

/*
 * The same, with a std::format message whose format string is
 * checked against its arguments at compile time. Returns -1.
 */
template <typename... Args>
int set_error(sq_errnum_t e, const char *sqlstate, int pos,
              std::format_string<Args...> fmt, Args &&...args) {
  return set_error_v(e, sqlstate, pos, fmt.get(),
                     std::make_format_args(args...));
}

/*
 * Add a detail and a hint to the error just recorded; either may be
 * NULL. set_error clears both.
 */
void set_error_extra(const char *detail, const char *hint);

/* Thrown by raise(), once the thread's error is recorded. */
struct parse_error_t {};

/* What kind of error to raise, and where. */
struct raise_t {
  const char *sqlstate;
  int pos = 0; /* in characters from 1, or 0 for none */
  const char *detail = nullptr;
  const char *hint = nullptr;
};

/*
 * Record a SQ_ERR_PARSE error, with a std::format message, and leave
 * the parse.
 */
template <typename... Args>
[[noreturn]] void raise(const raise_t &how, std::format_string<Args...> fmt,
                        Args &&...args) {
  set_error_v(SQ_ERR_PARSE, how.sqlstate, how.pos, fmt.get(),
              std::make_format_args(args...));
  set_error_extra(how.detail, how.hint);
  throw parse_error_t();
}

} // namespace sq

#endif /* SQ_SRC_ERROR_H */
