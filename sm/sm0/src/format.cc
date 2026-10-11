/*
 * format.cc -- sm0::format_into: std::format into a fixed buffer
 * through a bounded output iterator, so nothing is allocated.
 */

#include "sm0_format.h"

#include <cstddef>

namespace {

/*
 * An output iterator writing into a fixed buffer and silently
 * dropping whatever does not fit, so std::vformat_to can fill
 * a stack or thread-local buffer without allocating.
 */
class bounded_out_t {
public:
  using difference_type = std::ptrdiff_t;

  /* Write into [p, end). */
  bounded_out_t(char *p, char *end) : m_p(p), m_end(end) {}

  /* Dereference to self, so `*it = c` calls operator=. */
  bounded_out_t &operator*() { return *this; }

  /* Store c if there is room; drop it otherwise. */
  bounded_out_t &operator=(char c) {
    if (m_p < m_end) {
      *m_p++ = c;
    }
    return *this;
  }

  /* Advancing is done by operator=. */
  bounded_out_t &operator++() { return *this; }

  /* Advancing is done by operator=. */
  bounded_out_t operator++(int) { return *this; }

  /* Where the next character would go. */
  char *pos() const { return m_p; }

private:
  char *m_p;
  char *m_end;
};

} // namespace

namespace sm0 {

/* Format into buf through a bounded iterator, then terminate it. */
size_t format_into(char *buf, size_t size, std::string_view fmt,
                   std::format_args args) {
  // Leave room for the terminating NUL.
  bounded_out_t out(buf, buf + size - 1);
  // Format, dropping whatever does not fit.
  char *end = std::vformat_to(out, fmt, args).pos();
  // Terminate the message.
  *end = '\0';
  return end - buf;
}

} // namespace sm0
