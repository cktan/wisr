/*
 * utf8.h -- what the scanner needs to know about UTF-8.
 *
 * sq reads UTF-8 and nothing else. These are PostgreSQL's functions
 * of the same purpose (common/wchar.c, utils/mb/mbutils.c), for that
 * one encoding, and they agree with PostgreSQL byte for byte on text
 * that is not valid UTF-8 as well as on text that is.
 */
#ifndef SQ_SRC_UTF8_H
#define SQ_SRC_UTF8_H

#include <cstdint>

namespace sq {

/* The longest UTF-8 character, in bytes. */
inline constexpr int utf8_max_len = 4;

/* Whether c is a code point at all: not 0, and not past U+10FFFF. */
inline bool is_valid_unicode_codepoint(uint32_t c) {
  return c > 0 && c <= 0x10FFFF;
}

/* Whether c is the first half of a UTF-16 surrogate pair. */
inline bool is_utf16_surrogate_first(uint32_t c) {
  return c >= 0xD800 && c <= 0xDBFF;
}

/* Whether c is the second half of a UTF-16 surrogate pair. */
inline bool is_utf16_surrogate_second(uint32_t c) {
  return c >= 0xDC00 && c <= 0xDFFF;
}

/* The code point a surrogate pair stands for. */
inline uint32_t surrogate_pair_to_codepoint(uint32_t first, uint32_t second) {
  return ((first & 0x3FF) << 10) + 0x10000 + (second & 0x3FF);
}

/*
 * The length in bytes of the character that begins with the byte at
 * s, going by that byte alone: 1 for a byte that begins no character.
 */
int utf8_mblen(const unsigned char *s);

/*
 * Write code point c at buf as UTF-8, NUL-terminated; buf needs
 * utf8_max_len + 1 bytes. Returns the length written.
 */
int unicode_to_utf8(uint32_t c, unsigned char *buf);

/*
 * The number of characters in the first limit bytes at s, stopping at
 * a NUL: what turns a byte offset into a position in characters.
 */
int utf8_strlen_with_len(const char *s, int limit);

/*
 * The longest prefix of the len bytes at s that is no more than limit
 * bytes and ends between characters.
 */
int utf8_cliplen(const char *s, int len, int limit);

/*
 * Check that the len bytes at s are valid UTF-8 with no NUL among
 * them. Returns len, or the offset of the first byte that is not.
 */
int utf8_verify(const char *s, int len);

} // namespace sq

#endif /* SQ_SRC_UTF8_H */
