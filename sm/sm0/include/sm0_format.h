/*
 * sm0_format.h -- std::format into a fixed buffer, allocating
 * nothing: what an error or a log message needs when memory may
 * have run out.
 */
#ifndef SM0_FORMAT_H
#define SM0_FORMAT_H

#include <cstddef>
#include <format>
#include <string_view>

namespace sm0 {

/*
 * Format fmt with args into buf, a buffer of size bytes, cutting
 * the message short if it does not fit; always NUL-terminates.
 * Allocates nothing. Returns the length written.
 */
size_t format_into(char *buf, size_t size, std::string_view fmt,
                   std::format_args args);

} // namespace sm0

#endif /* SM0_FORMAT_H */
