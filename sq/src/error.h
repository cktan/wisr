/*
 * error.h -- setting the calling thread's error state.
 *
 * set_error records an error for sq_errnum(), sq_errmsg(),
 * sq_sqlstate() and sq_errpos() and returns -1, so that a call can
 * write `return set_error(...)`. Nothing here clears the state: a
 * successful call leaves it untouched (sq.h).
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

} // namespace sq

#endif /* SQ_SRC_ERROR_H */
