/*
 * utf8.cc -- PostgreSQL's UTF-8 functions, as the scanner uses them.
 */

#include "utf8.h"

namespace sq {

/* PostgreSQL's pg_utf_mblen. */
int utf8_mblen(const unsigned char *s) {
  if ((*s & 0x80) == 0) {
    return 1;
  }
  if ((*s & 0xe0) == 0xc0) {
    return 2;
  }
  if ((*s & 0xf0) == 0xe0) {
    return 3;
  }
  if ((*s & 0xf8) == 0xf0) {
    return 4;
  }
  return 1;
}

/* PostgreSQL's unicode_to_utf8, and the terminator its callers add. */
int unicode_to_utf8(uint32_t c, unsigned char *buf) {
  int len;
  if (c <= 0x7F) {
    buf[0] = c;
    len = 1;
  } else if (c <= 0x7FF) {
    buf[0] = 0xC0 | ((c >> 6) & 0x1F);
    buf[1] = 0x80 | (c & 0x3F);
    len = 2;
  } else if (c <= 0xFFFF) {
    buf[0] = 0xE0 | ((c >> 12) & 0x0F);
    buf[1] = 0x80 | ((c >> 6) & 0x3F);
    buf[2] = 0x80 | (c & 0x3F);
    len = 3;
  } else {
    buf[0] = 0xF0 | ((c >> 18) & 0x07);
    buf[1] = 0x80 | ((c >> 12) & 0x3F);
    buf[2] = 0x80 | ((c >> 6) & 0x3F);
    buf[3] = 0x80 | (c & 0x3F);
    len = 4;
  }
  buf[len] = '\0';
  return len;
}

/* PostgreSQL's pg_mbstrlen_with_len. */
int utf8_strlen_with_len(const char *s, int limit) {
  int len = 0;
  while (limit > 0 && *s) {
    int l = utf8_mblen(reinterpret_cast<const unsigned char *>(s));
    limit -= l;
    s += l;
    len++;
  }
  return len;
}

/* PostgreSQL's pg_mbcliplen. */
int utf8_cliplen(const char *s, int len, int limit) {
  int clen = 0;
  while (len > 0 && *s) {
    int l = utf8_mblen(reinterpret_cast<const unsigned char *>(s));
    if (clen + l > limit) {
      break;
    }
    clen += l;
    if (clen == limit) {
      break;
    }
    len -= l;
    s += l;
  }
  return clen;
}

namespace {

/* PostgreSQL's pg_utf8_islegal: whether length bytes are one character. */
bool utf8_islegal(const unsigned char *source, int length) {
  unsigned char a;

  switch (length) {
  default:
    return false;
  case 4:
    a = source[3];
    if (a < 0x80 || a > 0xBF) {
      return false;
    }
    [[fallthrough]];
  case 3:
    a = source[2];
    if (a < 0x80 || a > 0xBF) {
      return false;
    }
    [[fallthrough]];
  case 2:
    a = source[1];
    switch (*source) {
    case 0xE0:
      if (a < 0xA0 || a > 0xBF) {
        return false;
      }
      break;
    case 0xED:
      if (a < 0x80 || a > 0x9F) {
        return false;
      }
      break;
    case 0xF0:
      if (a < 0x90 || a > 0xBF) {
        return false;
      }
      break;
    case 0xF4:
      if (a < 0x80 || a > 0x8F) {
        return false;
      }
      break;
    default:
      if (a < 0x80 || a > 0xBF) {
        return false;
      }
      break;
    }
    [[fallthrough]];
  case 1:
    a = *source;
    if (a >= 0x80 && a < 0xC2) {
      return false;
    }
    if (a > 0xF4) {
      return false;
    }
    break;
  }
  return true;
}

} // namespace

/* PostgreSQL's pg_utf8_verifystr, a character at a time. */
int utf8_verify(const char *s, int len) {
  const unsigned char *p = reinterpret_cast<const unsigned char *>(s);
  int done = 0;
  while (done < len) {
    if (p[done] == '\0') {
      break;
    }
    int l = utf8_mblen(p + done);
    if (l > len - done || !utf8_islegal(p + done, l)) {
      break;
    }
    done += l;
  }
  return done;
}

} // namespace sq
