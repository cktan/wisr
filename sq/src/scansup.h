/*
 * scansup.h -- what the scanner does to an identifier and to a
 * number, apart from finding them: PostgreSQL's scansup.c, and the
 * part of its numutils.c the scanner calls.
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 * Portions Copyright (c) 2026, CK Tan: the changes for sq,
 * under the MIT License (https://github.com/cktan/wisr/blob/main/LICENSE)
 */
#ifndef SQ_SRC_SCANSUP_H
#define SQ_SRC_SCANSUP_H

#include <cstdint>

namespace sq {

struct scan_extra_t;

/* An identifier is at most this many bytes, less one: NAMEDATALEN. */
inline constexpr int namedatalen = 64;

/*
 * Copy the len bytes at ident into the tree with ASCII letters
 * lowered, and cut the copy to namedatalen - 1 bytes if it is longer,
 * with a notice. Returns the copy.
 */
char *downcase_truncate_identifier(scan_extra_t &extra, const char *ident,
                                   int len);

/*
 * Cut ident, of len bytes, to namedatalen - 1 bytes in place if it is
 * longer, between characters, with a notice.
 */
void truncate_identifier(scan_extra_t &extra, char *ident, int len);

/*
 * Read text as a 32-bit integer: decimal, or hexadecimal, octal or
 * binary by its 0x, 0o or 0b, with single underscores between digits.
 * Returns false if it is not one or does not fit.
 */
bool parse_int32(const char *text, int32_t *out);

} // namespace sq

#endif /* SQ_SRC_SCANSUP_H */
