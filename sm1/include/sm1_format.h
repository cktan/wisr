/*
 * sm1_format.h -- std::format into a fixed buffer, allocating
 * nothing: what an error or a log message needs when memory may
 * have run out.
 */
#ifndef SM1_FORMAT_H
#define SM1_FORMAT_H

#include <cstddef>
#include <format>
#include <string_view>

namespace sm1 {

/*
 * Format fmt with args into buf, a buffer of size bytes, cutting
 * the message short if it does not fit; always NUL-terminates.
 * Allocates nothing. Returns the length written.
 */
size_t format_into(char *buf, size_t size, std::string_view fmt,
                   std::format_args args);

} // namespace sm1

#endif /* SM1_FORMAT_H */
