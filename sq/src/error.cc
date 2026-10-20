/*
 * error.cc -- the per-thread error state behind sq_errnum(),
 * sq_errmsg(), sq_sqlstate(), sq_errpos(), sq_errdetail() and
 * sq_errhint().
 *
 * The message is formatted straight into a thread-local buffer
 * through a bounded output iterator: nothing is allocated, so an
 * error can be recorded when memory has run out.
 */

#include "error.h"

#include <string.h>

#include <cstddef>

#include "sq.h"

namespace {

constexpr size_t errmsg_size = 1024;
constexpr size_t errextra_size = 256;
constexpr size_t sqlstate_len = 5;

thread_local sq_errnum_t t_errnum = SQ_OK;
thread_local char t_errmsg[errmsg_size] = "no error";
thread_local char t_sqlstate[sqlstate_len + 1] = "00000";
thread_local int t_errpos = 0;
thread_local char t_errdetail[errextra_size] = "";
thread_local char t_errhint[errextra_size] = "";

/* Copy text into a buffer of size bytes, cut to fit; NULL is "". */
void copy_text(char *buf, size_t size, const char *text) {
  size_t n = text ? strlen(text) : 0;
  if (n > size - 1) {
    n = size - 1;
  }
  if (n > 0) {
    memcpy(buf, text, n);
  }
  buf[n] = '\0';
}

/*
 * An output iterator writing into a fixed buffer and silently
 * dropping whatever does not fit, so std::vformat_to can fill a
 * thread-local buffer without allocating.
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

namespace sq {

/* Record e, its SQLSTATE and position, and format its message. */
int set_error_v(sq_errnum_t e, const char *sqlstate, int pos,
                std::string_view fmt, std::format_args args) {
  // Format what fits, leaving room for the terminating NUL.
  bounded_out_t out(t_errmsg, t_errmsg + sizeof(t_errmsg) - 1);
  *std::vformat_to(out, fmt, args).pos() = '\0';
  // A SQLSTATE is always five characters.
  memcpy(t_sqlstate, sqlstate, sqlstate_len);
  t_sqlstate[sqlstate_len] = '\0';
  // An error has no detail or hint until it is given one.
  t_errdetail[0] = '\0';
  t_errhint[0] = '\0';
  // Record the code and the position.
  t_errpos = pos;
  t_errnum = e;
  return -1;
}

/* Record the detail and hint of the error just set. */
void set_error_extra(const char *detail, const char *hint) {
  copy_text(t_errdetail, sizeof(t_errdetail), detail);
  copy_text(t_errhint, sizeof(t_errhint), hint);
}

} // namespace sq

/* The last error raised on this thread. */
sq_errnum_t sq_errnum(void) { return t_errnum; }

/* The message for the last error raised on this thread. */
const char *sq_errmsg(void) { return t_errmsg; }

/* The SQLSTATE of the last error raised on this thread. */
const char *sq_sqlstate(void) { return t_sqlstate; }

/* The position of the last error raised on this thread, or 0. */
int sq_errpos(void) { return t_errpos; }

/* The detail of the last error raised on this thread, or "". */
const char *sq_errdetail(void) { return t_errdetail; }

/* The hint of the last error raised on this thread, or "". */
const char *sq_errhint(void) { return t_errhint; }

/* The flags the library was built with; see sq.h. */
const char *sq_build_flags(void) {
#ifdef SQ_BUILD_FLAGS
  return SQ_BUILD_FLAGS;
#else
  return "unknown";
#endif
}
